/* cmd_run.h — execution command for single-file and projects. */
#ifndef SNOVAC_CMD_RUN_H
#define SNOVAC_CMD_RUN_H

int cmd_run(const char *path);
int cmd_run_project(const char *path);
int cmd_run_project_with_cache(const char *path, const char *offline_cache);

#endif /* SNOVAC_CMD_RUN_H */
