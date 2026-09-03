/* cmd_build.h — standalone native compilation command. */
#ifndef SNOVAC_CMD_BUILD_H
#define SNOVAC_CMD_BUILD_H

int cmd_build(const char *path, const char *out_path, const char *target_override);
int cmd_build_project(const char *path, const char *out_path, const char *target_override, const char *offline_cache, int include_runtime);

#endif /* SNOVAC_CMD_BUILD_H */
