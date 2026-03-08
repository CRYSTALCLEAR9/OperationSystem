/*
 * A lightweight shell that can execute commands from a script file.
 * Supports background execution, a single pipe, history, and simple env vars.
 */

#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

#define MAXBUF 4096
#define MAX_LINE 256
#define MAX_HISTORY 64
#define MAX_ENV 16
#define MAX_BG 16
#define MAX_TOKENS 8

typedef struct shell_env_t {
  int used;
  char key[32];
  char value[128];
} shell_env;

typedef struct shell_state_t {
  shell_env envs[MAX_ENV];
  char history[MAX_HISTORY][MAX_LINE];
  int history_count;
  int bg_pids[MAX_BG];
  int bg_count;
} shell_state;

static shell_state *g_state = 0;

static void trim(char *line) {
  int len = strlen(line);
  int start = 0;
  while (line[start] == ' ' || line[start] == '\t' || line[start] == '\r') start++;
  while (len > start &&
         (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r'))
    len--;

  if (start > 0) memmove(line, line + start, len - start);
  line[len - start] = '\0';
}

static void add_history(const char *line) {
  if (line[0] == '\0') return;

  if (g_state->history_count < MAX_HISTORY) {
    safestrcpy(g_state->history[g_state->history_count], line, MAX_LINE);
    g_state->history_count++;
    return;
  }

  for (int i = 1; i < MAX_HISTORY; i++)
    safestrcpy(g_state->history[i - 1], g_state->history[i], MAX_LINE);
  safestrcpy(g_state->history[MAX_HISTORY - 1], line, MAX_LINE);
}

static int tokenize(char *line, char *tokens[], int max_tokens) {
  int count = 0;
  char *token = strtok(line, " \t");
  while (token && count < max_tokens) {
    tokens[count++] = token;
    token = strtok(NULL, " \t");
  }
  return count;
}

static int find_env(const char *key) {
  for (int i = 0; i < MAX_ENV; i++) {
    if (g_state->envs[i].used && strcmp(g_state->envs[i].key, key) == 0) return i;
  }
  return -1;
}

static const char *get_env_value(const char *key) {
  int index = find_env(key);
  return index >= 0 ? g_state->envs[index].value : "";
}

static void set_env_value(const char *key, const char *value) {
  int index = find_env(key);
  if (index < 0) {
    for (int i = 0; i < MAX_ENV; i++) {
      if (!g_state->envs[i].used) {
        index = i;
        g_state->envs[i].used = 1;
        break;
      }
    }
  }
  if (index < 0) {
    printu("setenv failed: env table is full\n");
    return;
  }

  safestrcpy(g_state->envs[index].key, key, sizeof(g_state->envs[index].key));
  safestrcpy(g_state->envs[index].value, value, sizeof(g_state->envs[index].value));
}

static void expand_token(const char *src, char *dst, int dst_size) {
  int di = 0;
  while (*src && di < dst_size - 1) {
    if (*src != '$') {
      dst[di++] = *src++;
      continue;
    }

    src++;
    char key[32];
    int ki = 0;
    while ((*src == '_' || (*src >= '0' && *src <= '9') || (*src >= 'a' && *src <= 'z') ||
            (*src >= 'A' && *src <= 'Z')) &&
           ki < (int)sizeof(key) - 1) {
      key[ki++] = *src++;
    }
    key[ki] = '\0';

    if (ki == 0) {
      dst[di++] = '$';
      continue;
    }

    const char *value = get_env_value(key);
    while (*value && di < dst_size - 1) dst[di++] = *value++;
  }

  dst[di] = '\0';
}

static void wait_background_jobs(int verbose) {
  for (int i = 0; i < g_state->bg_count; i++) {
    wait(g_state->bg_pids[i]);
    if (verbose) printu("[bg] process %d finished\n", g_state->bg_pids[i]);
  }
  g_state->bg_count = 0;
}

static int run_command(const char *command, const char *arg, int background) {
  int pid = fork();
  if (pid == 0) {
    if (exec(command, arg) == -1) {
      printu("exec failed: %s %s\n", command, arg ? arg : "");
      exit(-1);
    }
    exit(0);
  }

  if (background) {
    if (g_state->bg_count < MAX_BG) {
      g_state->bg_pids[g_state->bg_count++] = pid;
    } else {
      wait(pid);
    }
    return 0;
  }

  wait(pid);
  return 0;
}

static int run_pipeline(const char *left_cmd, const char *left_arg,
                        const char *right_cmd, const char *right_arg) {
  int fds[2];
  if (pipe_u(fds) < 0) {
    printu("pipe failed\n");
    return -1;
  }

  int producer = fork();
  if (producer == 0) {
    close(fds[0]);
    set_stdout(fds[1]);
    if (exec(left_cmd, left_arg) == -1) {
      printu("exec failed: %s %s\n", left_cmd, left_arg ? left_arg : "");
      exit(-1);
    }
    exit(0);
  }

  close(fds[1]);
  wait(producer);

  int consumer = fork();
  if (consumer == 0) {
    set_stdin(fds[0]);
    if (exec(right_cmd, right_arg) == -1) {
      printu("exec failed: %s %s\n", right_cmd, right_arg ? right_arg : "");
      exit(-1);
    }
    exit(0);
  }

  close(fds[0]);
  wait(consumer);
  return 0;
}

static int handle_builtin(int ntok, char *tokens[]) {
  if (strcmp(tokens[0], "history") == 0) {
    for (int i = 0; i < g_state->history_count; i++) printu("%d %s\n", i, g_state->history[i]);
    return 1;
  }

  if (strcmp(tokens[0], "setenv") == 0) {
    if (ntok < 3) {
      printu("usage: setenv KEY VALUE\n");
      return 1;
    }
    char value[128];
    expand_token(tokens[2], value, sizeof(value));
    set_env_value(tokens[1], value);
    return 1;
  }

  if (strcmp(tokens[0], "printenv") == 0) {
    if (ntok < 2) {
      printu("usage: printenv KEY\n");
      return 1;
    }
    printu("%s=%s\n", tokens[1], get_env_value(tokens[1]));
    return 1;
  }

  if (strcmp(tokens[0], "pwd") == 0) {
    char cwd[64];
    read_cwd(cwd);
    printu("cwd:%s\n", cwd);
    return 1;
  }

  if (strcmp(tokens[0], "cd") == 0) {
    if (ntok < 2 || change_cwd(tokens[1]) != 0) printu("cd failed\n");
    return 1;
  }

  if (strcmp(tokens[0], "waitall") == 0) {
    wait_background_jobs(1);
    return 1;
  }

  return 0;
}

int main(int argc, char *argv[]) {
  char *script = argc > 0 ? argv[0] : "/shellrc";
  printu("======== Shell Start ========\n\n");

  g_state = (shell_state *)better_malloc(sizeof(shell_state));
  if (g_state == 0) {
    printu("alloc shell state failed\n");
    exit(-1);
  }
  memset(g_state, 0, sizeof(shell_state));

  int fd = open(script, O_RDONLY);
  if (fd < 0) {
    printu("open script failed: %s\n", script);
    exit(-1);
  }

  char *buf = (char *)naive_malloc();
  if (buf == 0) {
    printu("alloc shell buffer failed\n");
    exit(-1);
  }
  int len = read_u(fd, buf, MAXBUF - 1);
  close(fd);
  if (len < 0) {
    printu("read script failed: %s\n", script);
    exit(-1);
  }
  buf[len] = '\0';

  char *cursor = buf;
  while (*cursor) {
    char raw_line[MAX_LINE];
    int li = 0;
    while (*cursor && *cursor != '\n' && li < MAX_LINE - 1) raw_line[li++] = *cursor++;
    raw_line[li] = '\0';
    if (*cursor == '\n') cursor++;

    char *comment = strchr(raw_line, '#');
    if (comment) *comment = '\0';
    trim(raw_line);
    if (raw_line[0] == '\0') continue;

    add_history(raw_line);

    char line[MAX_LINE];
    safestrcpy(line, raw_line, sizeof(line));
    char *tokens[MAX_TOKENS];
    int ntok = tokenize(line, tokens, MAX_TOKENS);
    if (ntok == 0) continue;

    if (ntok >= 2 && strcmp(tokens[0], "END") == 0 && strcmp(tokens[1], "END") == 0) break;

    int background = 0;
    if (strcmp(tokens[ntok - 1], "&") == 0) {
      background = 1;
      ntok--;
      if (ntok == 0) continue;
    }

    if (handle_builtin(ntok, tokens)) continue;

    int pipe_pos = -1;
    for (int i = 0; i < ntok; i++) {
      if (strcmp(tokens[i], "|") == 0) {
        pipe_pos = i;
        break;
      }
    }

    if (pipe_pos > 0 && pipe_pos < ntok - 1) {
      char left_cmd[128], left_arg[128], right_cmd[128], right_arg[128];
      if (pipe_pos > 2 || ntok - pipe_pos - 1 > 2 || background) {
        printu("unsupported pipeline: %s\n", raw_line);
        continue;
      }

      expand_token(tokens[0], left_cmd, sizeof(left_cmd));
      if (pipe_pos == 2)
        expand_token(tokens[1], left_arg, sizeof(left_arg));
      else
        left_arg[0] = '\0';

      expand_token(tokens[pipe_pos + 1], right_cmd, sizeof(right_cmd));
      if (ntok - pipe_pos - 1 == 2)
        expand_token(tokens[pipe_pos + 2], right_arg, sizeof(right_arg));
      else
        right_arg[0] = '\0';

      printu("Next command: %s %s | %s %s\n\n", left_cmd, left_arg, right_cmd, right_arg);
      printu("==========Command Start============\n\n");
      run_pipeline(left_cmd, left_arg[0] ? left_arg : 0, right_cmd, right_arg[0] ? right_arg : 0);
      printu("==========Command End============\n\n");
      continue;
    }

    char command[128];
    char arg[128];
    expand_token(tokens[0], command, sizeof(command));
    if (ntok >= 2)
      expand_token(tokens[1], arg, sizeof(arg));
    else
      arg[0] = '\0';

    printu("Next command: %s %s%s\n\n", command, arg, background ? " &" : "");
    printu("==========Command Start============\n\n");
    run_command(command, arg[0] ? arg : 0, background);
    if (!background) printu("==========Command End============\n\n");
  }

  wait_background_jobs(1);
  exit(0);
  return 0;
}
