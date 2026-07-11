/*******************************************************************************************
 *   b_project.c - Project definition (see b_project.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_project.h"

#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void ProjectDefaults(BrushProject *p) {
  memset(p, 0, sizeof(*p));
  snprintf(p->name, sizeof(p->name), "Untitled");
  snprintf(p->scene, sizeof(p->scene), "assets/main.def");
}

bool BrushProjectLoad(BrushProject *p, const char *dir) {
  ProjectDefaults(p);
  char path[512];
  snprintf(path, sizeof(path), "%s/project.def", dir);
  // LoadFileText (not fopen) so a mounted release pak can serve it.
  char *text = LoadFileText(path);
  if (text == NULL) return false;

  char *cursor = text;
  while (*cursor != '\0') {
    char *s = cursor;
    char *nl = strchr(cursor, '\n');
    if (nl != NULL) { *nl = '\0'; cursor = nl + 1; }
    else cursor += strlen(cursor);
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || *s == '#') continue;
    int version;
    if (sscanf(s, "version %d", &version) == 1) {
      if (version != 1)
        TraceLog(LOG_WARNING, "BRUSH project: %s is version %d (engine reads 1)",
                 path, version);
    } else if (strncmp(s, "name ", 5) == 0) {
      snprintf(p->name, sizeof(p->name), "%s", s + 5);
    } else if (strncmp(s, "scene ", 6) == 0) {
      snprintf(p->scene, sizeof(p->scene), "%s", s + 6);
    } else {
      TraceLog(LOG_WARNING, "BRUSH project: %s unknown line: %.40s", path, s);
    }
  }
  UnloadFileText(text);
  return true;
}

bool BrushProjectSave(const BrushProject *p, const char *dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/project.def", dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    TraceLog(LOG_WARNING, "BRUSH project: cannot write %s", path);
    return false;
  }
  fprintf(f, "# brush project definition (see engine/b_project.h)\n");
  fprintf(f, "version 1\n");
  fprintf(f, "name %s\n", p->name);
  fprintf(f, "scene %s\n", p->scene);
  fclose(f);
  return true;
}

bool BrushProjectBoot(BrushProject *p, int argc, char **argv) {
  const char *dir = getenv("BRUSH_PROJECT");
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], "--project") == 0) dir = argv[i + 1];

  if (dir != NULL && dir[0] != '\0') {
    if (chdir(dir) != 0) {
      TraceLog(LOG_WARNING, "BRUSH project: cannot enter %s — staying in cwd",
               dir);
    } else {
      TraceLog(LOG_INFO, "BRUSH project: root %s", dir);
    }
  }
  bool ok = BrushProjectLoad(p, ".");
  if (ok)
    TraceLog(LOG_INFO, "BRUSH project: '%s' (scene %s)", p->name, p->scene);
  return ok;
}
