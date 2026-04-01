#ifndef LUMENCODE_BRIDGE_H
#define LUMENCODE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

char *lumencode_analyze_file_json(const char *path);
void lumencode_free_string(char *value);

#ifdef __cplusplus
}
#endif

#endif
