#pragma once

#define ENGINE_MAX_ARGS 32

typedef int (*enginecall_t)(const char *input_file, const char *output_file, int argc, char *argv[]);

/** Parse space-separated argv with single/double quoting; modifies buf in place. Returns argc or -1 on error. */
int engine_argv_parse(char *buf, char *argv[], int max_argv);

int engine_json_call(const char *input_file, const char *output_file, int argc, char *argv[]);
int engine_xpath_call(const char *input_file, const char *output_file, int argc, char *argv[]);
int engine_cp_call(const char *input_file, const char *output_file, int argc, char *argv[]);
int engine_lz4_call(const char *input_file, const char *output_file, int argc, char *argv[]);
int engine_rm_call(const char *input_file, const char *output_file, int argc, char *argv[]);
