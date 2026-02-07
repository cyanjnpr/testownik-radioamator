#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <curl/curl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <poppler/glib/poppler.h>
#include <stdio.h>
#define STB_DS_IMPLEMENTATION
#include "stb/stb_ds.h"

// read the pdf file with exam questions provided by UKE and convert it to WUST
// Testownik file format.

typedef struct {
  int number;
  int y;
  GString *question;
  GString *answer1;
  GString *answer2;
  GString *answer3;
  short int correct;
  gboolean has_image;
} Question;

typedef enum {
  UNKNOWN,
  QUESTION,
  ANSWER1,
  ANSWER2,
  ANSWER3,
  QUESTION_BODY
} TextPart;

typedef struct {
  int index;
  double x;
  double y;
} CharPos;

typedef struct {
  int index;
  gboolean is_bold;
  gdouble font_size;
} CharAttribute;

Question *exam = NULL;

gboolean is_font_bold(gchar *fontName) {
  return (g_strstr_len(g_utf8_casefold(fontName, -1), -1,
                       g_utf8_casefold("bold", -1))) != NULL;
}

int sort_characters(const void *a, const void *b) {
  CharPos *p1 = (CharPos *)a;
  CharPos *p2 = (CharPos *)b;

  if (fabs(p1->y - p2->y) >
      10) { // 10 is magic and if the code fails thay may be the reason
    if (p1->y < p2->y)
      return -1;
    if (p1->y > p2->y)
      return 1;
  }

  if (p1->x < p2->x)
    return -1;
  if (p1->x > p2->x)
    return 1;
  return 0;
}

gboolean zip_directory(const gchar *dir_path, const gchar *zip_path,
                       GError **error) {
  struct archive *a = archive_write_new();
  archive_write_set_format_zip(a);

  if (archive_write_open_filename(a, zip_path) != ARCHIVE_OK) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to create zip: %s", archive_error_string(a));
    archive_write_free(a);
    return FALSE;
  }
  GDir *dir = g_dir_open(dir_path, 0, error);
  if (!dir) {
    archive_write_free(a);
    return FALSE;
  }

  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    gchar *filepath = g_build_filename(dir_path, name, NULL);
    GStatBuf st;
    if (g_stat(filepath, &st) != 0) {
      g_free(filepath);
      continue;
    }

    struct archive_entry *ae = archive_entry_new();
    gchar *entry_path = g_build_filename("testownikradioamator", name, NULL);
    archive_entry_set_pathname(ae, entry_path);
    archive_entry_set_size(ae, st.st_size);
    archive_entry_set_filetype(ae, AE_IFREG);
    archive_entry_set_perm(ae, 0644);
    archive_write_header(a, ae);

    gchar *contents;
    gsize length;
    if (g_file_get_contents(filepath, &contents, &length, NULL)) {
      archive_write_data(a, contents, length);
      g_free(contents);
    }

    g_free(entry_path);
    archive_entry_free(ae);
    g_free(filepath);
  }

  g_dir_close(dir);
  archive_write_close(a);
  archive_write_free(a);
  return TRUE;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
  size_t written = fwrite(ptr, size, nmemb, stream);
  return written;
}

gchar *download_pdf(const gchar *url, GError **error) {
  const gchar *tmp_dir = g_get_tmp_dir();
  gchar *template = "radioamatorexamXXXXXX.pdf";
  gchar *filename = g_build_filename(tmp_dir, template, NULL);
  gint fd = g_mkstemp(filename);
  if (fd == -1) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to create temp file");
    return NULL;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to initialize curl");
    g_free(filename);
    return NULL;
  }

  FILE *fp = fdopen(fd, "wb");
  if (!fp) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to open temp file for writing");
    curl_easy_cleanup(curl);
    g_free(filename);
    return NULL;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  fclose(fp);

  if (res != CURLE_OK) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Download failed: %s",
                curl_easy_strerror(res));
    g_unlink(filename);
    g_free(filename);
    return NULL;
  }

  gchar *result = g_strdup_printf("%s%s", "file://", filename);
  g_free(filename);
  return result;
}

int main(int argc, char **argv) {
  GError *err = NULL;
  if (argc < 3) {
    g_printerr("Usage: %s <source> <target>\n", argv[0]);
    return 1;
  }
  gboolean pdf_is_temp = FALSE;
  char *source = argv[1];
  const char *target = argv[2];

  if (g_str_has_prefix(source, "http")) {
    source = download_pdf(source, &err);
    pdf_is_temp = TRUE;
    if (!source) {
      g_printerr("Error: %s\n", err->message);
      g_error_free(err);
      return 1;
    }
  } else if (!g_str_has_prefix(source, "file")) {
    perror("Source should be either http or file schema uri");
    return 1;
  }

  PopplerDocument *doc = poppler_document_new_from_file(source, NULL, &err);
  if (!doc) {
    g_printerr("Error: %s\n", err->message);
    g_error_free(err);
    return 1;
  }

  const gchar *tmp_dir = g_get_tmp_dir();
  char *exam_dir =
      g_mkdtemp(g_build_filename(tmp_dir, "testownikradioamatorXXXXXX", NULL));
  if (exam_dir == NULL) {
    perror("Failed to create temporary exam directory");
    return 1;
  }

  TextPart mode = UNKNOWN;
  int page_count = poppler_document_get_n_pages(doc);
  int current_question = 1;
  gdouble previous_font_size = 0;

  for (int p = 0; p < page_count; p++) {
    PopplerPage *page = poppler_document_get_page(doc, p);
    GList *attrs = poppler_page_get_text_attributes(page);
    char *text = poppler_page_get_text(page);
    GList *image_mapping = poppler_page_get_image_mapping(page);

    double page_width, page_height;
    poppler_page_get_size(page, &page_width, &page_height);

    PopplerRectangle *rectangles;
    guint chars_total;
    poppler_page_get_text_layout(page, &rectangles, &chars_total);

    // ---------- SORT THE TEXT AND ATTRIBUTES

    CharPos *positions = malloc(chars_total * sizeof(CharPos));
    int *reverse_index_map = malloc(chars_total * sizeof(int));
    // PopplerTextAttributes are grouped, this array holds separated attributes
    // for single chars
    CharAttribute *attributes = malloc(chars_total * sizeof(CharAttribute));
    int page_first_qi = (int)arrlen(exam);

    // https://stackoverflow.com/a/2740095
    // pdfs text order can be different from rendered order
    for (int i = 0; i < chars_total; i++) {
      positions[i].index = i;
      positions[i].x = rectangles[i].x2;
      // upper coord seems to be independent of char, while the lower is (spaces
      // are affected)
      positions[i].y = rectangles[i].y2;
    }

    // rebuild text in reading order
    qsort(positions, chars_total, sizeof(CharPos), sort_characters);
    GString *sorted = g_string_new("");
    for (int i = 0; i < chars_total; i++) {
      int idx = positions[i].index;

      gchar *char_ptr = g_utf8_offset_to_pointer(text, idx);
      gunichar next = g_utf8_get_char(char_ptr);
      gchar cbuf[6];
      gint clen = g_unichar_to_utf8(next, cbuf);
      g_string_append_len(sorted, cbuf, clen);
    }

    // break down attributes to single characters
    // only works if they do not overlap (they don't)
    for (GList *l = attrs; l; l = l->next) {
      PopplerTextAttributes *a = l->data;
      for (int i = a->start_index; i < a->end_index + 1; i++) {
        attributes[i].index = i;
        attributes[i].is_bold = is_font_bold(a->font_name);
        attributes[i].font_size = a->font_size;
      }
    }

    for (int i = 0; i < chars_total; i++) {
      reverse_index_map[positions[i].index] = i;
    }

    // sort attributes in reading order like the text
    for (int i = 0; i < chars_total; i++) {
      if (positions[i].index != attributes[i].index) {
        CharAttribute v = attributes[i];
        attributes[i] = attributes[reverse_index_map[i]];
        attributes[reverse_index_map[i]] = v;
      }
    }

    // ---------- ITERATE THROUGH THE TEXT

    gchar *gc = sorted->str;
    int ignore = 0;
    for (int i = 0; i < chars_total; i++) {
      if (previous_font_size < attributes[i].font_size &&
          attributes[i].is_bold) {
        // change of category
        current_question = 1;
        mode = UNKNOWN;
      }
      gchar *qp = g_strdup_printf("%d.", current_question);

      gunichar c = g_utf8_get_char(gc);
      if (c == '\n')
        c = (gunichar)' ';
      gchar cbuf[6];
      gint clen = g_unichar_to_utf8(c, cbuf);

      if (g_str_has_prefix(gc, qp)) {
        arrput(exam,
               ((Question){arrlen(exam), (int)positions[i].y, g_string_new(""),
                           g_string_new(""), g_string_new(""), g_string_new(""),
                           0, FALSE}));
        ignore = (int)log10(current_question) + 3;
        mode = QUESTION;
        current_question++;
      } else if (g_str_has_prefix(gc, "a. ") && mode == QUESTION_BODY) {
        ignore = 3;
        mode = ANSWER1;
      } else if (g_str_has_prefix(gc, "b. ") && mode == ANSWER1) {
        ignore = 3;
        mode = ANSWER2;
      } else if (g_str_has_prefix(gc, "c. ") && mode == ANSWER2) {
        ignore = 3;
        mode = ANSWER3;
      }

      if (ignore) {
        ignore--;
      } else if (arrlen(exam) > 0) {
        int qi = arrlen(exam) - 1;
        switch (mode) {
        case QUESTION:
        case QUESTION_BODY:
          g_string_append_len(exam[qi].question, cbuf, clen);
          break;
        case ANSWER1:
          if (attributes[i].is_bold)
            exam[qi].correct = 0b100;
          g_string_append_len(exam[qi].answer1, cbuf, clen);
          break;
        case ANSWER2:
          if (attributes[i].is_bold)
            exam[qi].correct = 0b010;
          g_string_append_len(exam[qi].answer2, cbuf, clen);
          break;
        case ANSWER3:
          if (attributes[i].is_bold)
            exam[qi].correct = 0b001;
          g_string_append_len(exam[qi].answer3, cbuf, clen);
          break;
        case UNKNOWN:
        }
      }

      if (mode == QUESTION && (c == ':' || c == '?')) {
        mode = QUESTION_BODY;
      }

      g_free(qp);
      gc = g_utf8_next_char(gc);
      previous_font_size = attributes[i].font_size;
    }

    // ---------- ITERATE THROUGH / EXPORT IMAGES

    for (GList *l = image_mapping; l != NULL; l = l->next) {
      PopplerImageMapping *m = l->data;

      int iy = (int)m->area.y2;
      int img_question = 0;
      for (int i = page_first_qi; i < arrlen(exam); i++) {
        if (i == page_first_qi) {
          if (exam[i].y > iy) {
            if (i > 0) {
              exam[i - 1].has_image = TRUE;
              img_question = exam[i - 1].number;
              break;
            }
          }
        } else if (exam[i - 1].y < iy && exam[i].y > iy) {
          exam[i - 1].has_image = TRUE;
          img_question = exam[i - 1].number;
          break;
        } else if (i == arrlen(exam) - 1 && exam[i].y < iy) {
          exam[i].has_image = TRUE;
          img_question = exam[i].number;
        }
      }

      gchar *name = g_strdup_printf("%03d.png", img_question);
      gchar *filename = g_build_filename(exam_dir, name, NULL);
      cairo_surface_t *img = poppler_page_get_image(page, m->image_id);
      cairo_surface_write_to_png(img, filename);
      cairo_surface_destroy(img);
      g_free(filename);
      g_free(name);
    }

    free(attributes);
    free(positions);
    free(reverse_index_map);
    g_free(rectangles);

    poppler_page_free_image_mapping(image_mapping);
    poppler_page_free_text_attributes(attrs);
    g_free(text);
    g_object_unref(page);
  }
  g_object_unref(doc);

  if (pdf_is_temp) {
    gchar *path = g_filename_from_uri(source, NULL, NULL);
    g_unlink(path);
    g_free(path);
    g_free(source);
  }

  // ---------- EXPORT QUESTIONS

  char answer_array[4];
  answer_array[3] = '\0';
  for (int i = 0; i < arrlen(exam); i++) {
    Question q = exam[i];
    gchar *name = g_strdup_printf("%03d.txt", q.number);
    gchar *filename = g_build_filename(exam_dir, name, NULL);

    for (int i = 2; i >= 0; i--) {
      answer_array[2 - i] = (q.correct & (1 << i)) ? '1' : '0';
    }
    gchar *contents;
    if (q.has_image) {
      contents = g_strdup_printf(
          "X%s\n[img]%03d.png[/img] %s\n%s\n%s\n%s", answer_array, q.number,
          q.question->str, q.answer1->str, q.answer2->str, q.answer3->str);
    } else {
      contents =
          g_strdup_printf("X%s\n%s\n%s\n%s\n%s", answer_array, q.question->str,
                          q.answer1->str, q.answer2->str, q.answer3->str);
    }

    if (!g_file_set_contents(filename, contents, -1, &err)) {
      g_printerr("Failed to write the exam question to file %s: %s\n", filename,
                 err->message);
      g_error_free(err);
      break;
    }

    g_free(name);
    g_free(filename);
    g_free(contents);
  }

  gboolean zip_status = zip_directory(exam_dir, target, &err);
  if (!zip_status) {
    g_printerr("Error: %s\n", err->message);
    g_error_free(err);
  }

  // ---------- CLEAR TEMP DIR

  for (int i = 0; i < arrlen(exam); i++) {
    Question q = exam[i];
    gchar *name = g_strdup_printf("%03d.txt", q.number);
    gchar *filename = g_build_filename(exam_dir, name, NULL);

    if (q.has_image) {
      gchar *name = g_strdup_printf("%03d.png", q.number);
      gchar *filename = g_build_filename(exam_dir, name, NULL);
      g_unlink(filename);
      g_free(name);
      g_free(filename);
    }

    g_unlink(filename);
    g_free(name);
    g_free(filename);
  }

  g_rmdir(exam_dir);
  g_free(exam_dir);
  return 0;
}
