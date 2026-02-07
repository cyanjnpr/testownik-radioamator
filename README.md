# Testownik Radioamator

Materiały pomocnicze do egzaminu radioamatorskiego w formacie Testownika używanego na Politechnice Wrocławskiej.

## Przeznaczenie

Repozytorium przeznaczone dla studentów PWr zainteresowanych zdobyciem uprawnień radioamatorskich.

## Wykorzystanie

Baza może zostać wczytana do jednej z wielu wersji aplikacji testownik dostępnych online. 
Przykładem może być [testownik Solvro](https://github.com/solvro/web-testownik) (Wymaga zalogowania do poprawnego zaimportowania quizu).

## Lokalizacja plików

Skompilowane archiwa ZIP umieszczone są w zakładce **Releases**.
![Files location screenshot](.github/files_location.png "Files location screenshot")

W przypadku pojawienia się nowej wersji bazy możliwa jest jej samodzielna kompilacja po zmianie zawartości pliku *categories.yaml*.

```bash
git clone https://github.com/cyanjnpr/testownik-radioamator/
cd testownik-radioamator/
make
bash release.sh
ls -l out/
```
