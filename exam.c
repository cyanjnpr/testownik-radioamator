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

// read the pdf file with exam questions provided by UKE and convert it to
// Testownik file format.

typedef struct {
  int index;
  double x1;
  double x2;
  double y1;
  double y2;
} CharPos;

static inline CharPos pos_scaled(CharPos pos, int scale) {
  pos.x1 *= scale;
  pos.x2 *= scale;
  pos.y1 *= scale;
  pos.y2 *= scale;
  return pos;
}

typedef struct {
  int number;
  CharPos q_pos;
  CharPos a1_pos;
  CharPos a2_pos;
  CharPos a3_pos;
  GString *question;
  GString *answer1;
  GString *answer2;
  GString *answer3;
  short int correct;
  gboolean confidently_correct;
  gboolean has_image;
  int image_count;
} Question;

typedef enum { UNKNOWN, QUESTION, ANSWER1, ANSWER2, ANSWER3 } TextPart;

typedef struct {
  int index;
  gboolean is_bold;
  gboolean is_underlined;
  gdouble font_size;
} CharAttribute;

Question *exam = NULL;

gboolean is_font_bold(gchar *fontName) {
  return (g_strstr_len(g_utf8_casefold(fontName, -1), -1,
                       g_utf8_casefold("bold", -1))) != NULL;
}

gboolean is_answer(int ax) {
  const int THRESHOLD = 5;
  static int counter = 0;
  static int sum = 0;
  if (counter == 0) {
    sum = ax;
    counter++;
    return TRUE;
  } else if (abs((sum / counter) - ax) < THRESHOLD) {
    sum += ax;
    counter++;
    return TRUE;
  }
  return FALSE;
}

gboolean is_question(int qx) {
  const int THRESHOLD = 5;
  static int counter = 0;
  static int sum = 0;
  if (counter == 0) {
    sum = qx;
    counter++;
    return TRUE;
  } else if (abs((sum / counter) - qx) < THRESHOLD) {
    sum += qx;
    counter++;
    return TRUE;
  }
  return FALSE;
}

gboolean is_paragraph_part(int font_size, TextPart m, CharPos *p1) {
  static int paragraph_start = 0;
  static TextPart lm = UNKNOWN;
  static CharPos *lp = NULL;
  if (lp == NULL || m != lm) {
    paragraph_start = p1->x1;
    lm = m;
    lp = p1;
    return TRUE;
  }
  return ((abs(p1->x1 - paragraph_start) < font_size * 2 && p1->y2 > lp->y2) ||
          (abs(p1->y2 - lp->y2) < font_size * 4 && p1->x2 > lp->x1));
}

void save_cropped_region(cairo_surface_t *source_surface, gchar *filename,
                         int top_y, int bottom_y, int page_width) {
  cairo_surface_t *crop = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, page_width, bottom_y - top_y);
  cairo_t *cr = cairo_create(crop);
  cairo_set_source_surface(cr, source_surface, 0, -top_y);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_write_to_png(crop, filename);
  cairo_surface_destroy(crop);
}

gboolean is_underlined_answer(cairo_surface_t *surface, CharPos a_pos) {
  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *data = cairo_image_surface_get_data(surface);
  int min_width = (int)((a_pos.x2 - a_pos.x1) * 0.95);

  int y_h = a_pos.y2 - a_pos.y1;
  for (int y = a_pos.y1; y < a_pos.y2 + y_h / 2; y++) {
    unsigned char *row = data + (y * stride);
    int consecutive_black = 0;

    for (int x = a_pos.x1; x < a_pos.x2; x++) {
      unsigned char *pixel = row + (x * 4); // BGRA
      unsigned char b = pixel[0];
      unsigned char g = pixel[1];
      unsigned char r = pixel[2];
      int thresh = 200;
      int is_black = (r < thresh && g < thresh && b < thresh);

      if (is_black) {
        consecutive_black++;
      } else {
        if (consecutive_black >= min_width) {
          return TRUE;
        }
        consecutive_black = 0;
      }
    }
    if (consecutive_black >= min_width) {
      return TRUE;
    }
  }
  return FALSE;
}

int sort_characters(const void *a, const void *b) {
  CharPos *p1 = (CharPos *)a;
  CharPos *p2 = (CharPos *)b;
  int THRESHOLD = 10;

  if (fabs(p1->y2 - p2->y2) > THRESHOLD) {
    if (p1->y2 < p2->y2)
      return -1;
    if (p1->y2 > p2->y2)
      return 1;
  }

  if (p1->x2 < p2->x2)
    return -1;
  if (p1->x2 > p2->x2)
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
  close(fd);

  CURL *curl = curl_easy_init();
  if (!curl) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to initialize curl");
    g_unlink(filename);
    g_free(filename);
    return NULL;
  }

  FILE *fp = fopen(filename, "wb");
  if (!fp) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Failed to open temp file for writing");
    curl_easy_cleanup(curl);
    g_unlink(filename);
    g_free(filename);
    return NULL;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
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

  int margin_top_y = INT32_MAX;
  int margin_bottom_y = 0;

  for (int p = 0; p < page_count; p++) {
    PopplerPage *page = poppler_document_get_page(doc, p);

    PopplerRectangle *rectangles;
    guint chars_total;
    poppler_page_get_text_layout(page, &rectangles, &chars_total);
    if (chars_total == 0) {
      g_object_unref(page);
      continue;
    }

    char *text = poppler_page_get_text(page);
    GList *attrs = poppler_page_get_text_attributes(page);
    GList *image_mapping = poppler_page_get_image_mapping(page);

    double page_width, page_height;
    poppler_page_get_size(page, &page_width, &page_height);

    double render_scale = 3;
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)(page_width * render_scale),
        (int)(page_height * render_scale));
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, render_scale, render_scale);
    poppler_page_render(page, cr);
    cairo_destroy(cr);

    // ---------- SORT THE TEXT AND ATTRIBUTES

    // https://stackoverflow.com/a/2740095
    // pdfs text order can be different from rendered order
    CharPos *positions = malloc(chars_total * sizeof(CharPos));
    int *reverse_index_map = malloc(chars_total * sizeof(int));
    // PopplerTextAttributes are grouped, this array holds separated attributes
    // for single chars
    CharAttribute *attributes = malloc(chars_total * sizeof(CharAttribute));
    int page_first_qi = (int)arrlen(exam);

    for (int i = 0; i < chars_total; i++) {
      positions[i].index = i;
      positions[i].x1 = rectangles[i].x1;
      positions[i].x2 = rectangles[i].x2;
      positions[i].y1 = rectangles[i].y1;
      positions[i].y2 = rectangles[i].y2;
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
    for (GList *l = attrs; l; l = l->next) {
      PopplerTextAttributes *a = l->data;
      for (int i = a->start_index; i < a->end_index + 1; i++) {
        attributes[i].index = i;
        attributes[i].is_bold = is_font_bold(a->font_name);
        attributes[i].is_underlined = a->is_underlined;
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
      if (positions[i].y2 < margin_top_y)
        margin_top_y = positions[i].y2;
      if (positions[i].y2 > margin_bottom_y)
        margin_bottom_y = positions[i].y2;

      if (previous_font_size < attributes[i].font_size &&
          attributes[i].is_bold && mode == ANSWER3) {
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

      if (g_str_has_prefix(gc, qp) && is_question(positions[i].x1)) {
        arrput(exam, ((Question){arrlen(exam), positions[i], positions[i],
                                 positions[i], positions[i], g_string_new(""),
                                 g_string_new(""), g_string_new(""),
                                 g_string_new(""), 0, FALSE, FALSE, 0}));
        ignore = (int)log10(current_question) + 2;
        exam[arrlen(exam) - 1].q_pos.y1 = positions[i].y2;
        mode = QUESTION;
        current_question++;
      } else if ((g_str_has_prefix(gc, "a.") || g_str_has_prefix(gc, "A.")) &&
                 mode == QUESTION && is_answer(positions[i].x1)) {
        ignore = 2;
        mode = ANSWER1;
        exam[arrlen(exam) - 1].a1_pos = positions[i + 3];
      } else if ((g_str_has_prefix(gc, "b.") || g_str_has_prefix(gc, "B.")) &&
                 mode == ANSWER1 && is_answer(positions[i].x1)) {
        ignore = 2;
        mode = ANSWER2;
        exam[arrlen(exam) - 1].a2_pos = positions[i + 3];
      } else if ((g_str_has_prefix(gc, "c.") || g_str_has_prefix(gc, "C.")) &&
                 mode == ANSWER2 && is_answer(positions[i].x1)) {
        ignore = 2;
        mode = ANSWER3;
        exam[arrlen(exam) - 1].a3_pos = positions[i + 3];
      }

      if (ignore) {
        ignore--;
      } else if (arrlen(exam) > 0) {
        int qi = arrlen(exam) - 1;
        switch (mode) {
        case QUESTION:
          if (is_paragraph_part(attributes[i].font_size, mode, &positions[i])) {
            g_string_append_len(exam[qi].question, cbuf, clen);
            exam[qi].q_pos.y2 = positions[i].y2;
          }
          break;
        case ANSWER1:
          if (attributes[i].is_bold || attributes[i].is_underlined) {
            exam[qi].correct = 0b100;
            exam[qi].confidently_correct = TRUE;
          }
          if (is_paragraph_part(attributes[i].font_size, mode, &positions[i])) {
            g_string_append_len(exam[qi].answer1, cbuf, clen);
            if (exam[qi].a1_pos.y2 == positions[i].y2) {
              exam[qi].a1_pos.x2 = positions[i].x2;
              // fallback, font doesn't carry information about underline,
              // stroke is a separate pdf object.
              // min number of characters is necessary to recognize the presence
              // of underline, sacrifice short questions
              if (!exam[qi].confidently_correct &&
                  strlen(exam[qi].answer1->str) > 3 &&
                  is_underlined_answer(
                      surface, pos_scaled(exam[qi].a1_pos, render_scale))) {
                exam[qi].correct = 0b100;
              }
            }
          }
          break;
        case ANSWER2:
          if (attributes[i].is_bold || attributes[i].is_underlined) {
            exam[qi].correct = 0b010;
            exam[qi].confidently_correct = TRUE;
          }
          if (is_paragraph_part(attributes[i].font_size, mode, &positions[i])) {
            g_string_append_len(exam[qi].answer2, cbuf, clen);
            if (exam[qi].a2_pos.y2 == positions[i].y2) {
              exam[qi].a2_pos.x2 = positions[i].x2;

              if (!exam[qi].confidently_correct &&
                  strlen(exam[qi].answer2->str) > 3 &&
                  is_underlined_answer(
                      surface, pos_scaled(exam[qi].a2_pos, render_scale))) {
                exam[qi].correct = 0b010;
              }
            }
          }
          break;
        case ANSWER3:
          if (attributes[i].is_bold || attributes[i].is_underlined) {
            exam[qi].correct = 0b001;
            exam[qi].confidently_correct = TRUE;
          }
          if (is_paragraph_part(attributes[i].font_size, mode, &positions[i])) {
            g_string_append_len(exam[qi].answer3, cbuf, clen);
            if (exam[qi].a3_pos.y2 == positions[i].y2) {
              exam[qi].a3_pos.x2 = positions[i].x2;

              if (!exam[qi].confidently_correct &&
                  strlen(exam[qi].answer3->str) > 3 &&
                  is_underlined_answer(
                      surface, pos_scaled(exam[qi].a3_pos, render_scale))) {
                exam[qi].correct = 0b001;
              }
            }
          }
          break;
        case UNKNOWN:
        }
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
          if (exam[i].q_pos.y1 > iy) {
            if (i > 0) {
              exam[i - 1].image_count++;
              // if image is made up of a few smaller images, extract it later as a screenshot
              exam[i - 1].has_image = exam[i - 1].image_count == 1;
              img_question = exam[i - 1].number;
              break;
            }
          }
        } else if (exam[i - 1].q_pos.y1 < iy && exam[i].q_pos.y1 > iy) {
          exam[i - 1].image_count++;
          exam[i - 1].has_image = exam[i - 1].image_count == 1;
          img_question = exam[i - 1].number;
          break;
        } else if (i == arrlen(exam) - 1 && exam[i].q_pos.y1 < iy) {
          exam[i].image_count++;
          exam[i].has_image = exam[i].image_count == 1;
          img_question = exam[i].number;
        }
      }

      if (img_question > 0) {
        gchar *name = g_strdup_printf("%03d.png", img_question);
        gchar *filename = g_build_filename(exam_dir, name, NULL);
        cairo_surface_t *img = poppler_page_get_image(page, m->image_id);
        cairo_surface_write_to_png(img, filename);
        cairo_surface_destroy(img);
        g_free(filename);
        g_free(name);
      }
    }

    // figures made up of strokes and shapes
    for (int i = fmax(page_first_qi - 1, 0); i < arrlen(exam); i++) {
      Question q = exam[i];
      if (!q.has_image) {
        if (i == page_first_qi - 1) {
          if (exam[i].q_pos.y2 > exam[i].a1_pos.y2 &&
              exam[i].a1_pos.y2 - margin_top_y > previous_font_size * 3) {
            exam[i].has_image = TRUE;
            exam[i].image_count++;
            gchar *name = g_strdup_printf("%03d.png", exam[i].number);
            gchar *filename = g_build_filename(exam_dir, name, NULL);
            save_cropped_region(surface, filename, margin_top_y * render_scale,
                                q.a1_pos.y1 * render_scale,
                                (int)page_width * render_scale);
            g_free(filename);
            g_free(name);
          } else if (exam[i].q_pos.y2 > exam[i].a1_pos.y2 &&
                     margin_bottom_y - exam[i].q_pos.y2 >
                         previous_font_size * 3) {
            exam[i].has_image = TRUE;
          }
        } else if (q.q_pos.y2 < q.a1_pos.y2 &&
                   q.a1_pos.y2 - q.q_pos.y2 > previous_font_size * 3) {
          exam[i].has_image = TRUE;
          exam[i].image_count++;
          gchar *name = g_strdup_printf("%03d.png", q.number);
          gchar *filename = g_build_filename(exam_dir, name, NULL);
          save_cropped_region(surface, filename, q.q_pos.y1 * render_scale,
                              q.a1_pos.y1 * render_scale,
                              (int)page_width * render_scale);
          g_free(filename);
          g_free(name);
        } else if (i == arrlen(exam) - 1 &&
                   exam[i].q_pos.y2 > exam[i].a1_pos.y2 &&
                   margin_bottom_y - exam[i].q_pos.y2 >
                       previous_font_size * 3) {
          exam[i].image_count++;
          gchar *name = g_strdup_printf("%03d.png", exam[i].number);
          gchar *filename = g_build_filename(exam_dir, name, NULL);
          save_cropped_region(surface, filename, q.q_pos.y1 * render_scale,
                              margin_bottom_y * render_scale,
                              (int)page_width * render_scale);
          g_free(filename);
          g_free(name);
        }
      }
    }

    free(attributes);
    free(positions);
    free(reverse_index_map);
    g_free(rectangles);

    cairo_surface_destroy(surface);

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
    g_strstrip(q.question->str);
    g_strstrip(q.answer1->str);
    g_strstrip(q.answer2->str);
    g_strstrip(q.answer3->str);
    if (q.correct == 0) {
      continue;
    }
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

    if (q.image_count > 0) {
      gchar *name = g_strdup_printf("%03d.png", q.number);
      gchar *filename = g_build_filename(exam_dir, name, NULL);
      g_unlink(filename);
      g_free(name);
      g_free(filename);
    }

    g_string_free(q.question, TRUE);
    g_string_free(q.answer1, TRUE);
    g_string_free(q.answer2, TRUE);
    g_string_free(q.answer3, TRUE);
    g_unlink(filename);
    g_free(name);
    g_free(filename);
  }

  arrfree(exam);
  g_rmdir(exam_dir);
  g_free(exam_dir);
  return 0;
}
