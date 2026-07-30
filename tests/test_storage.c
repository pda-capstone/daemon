#include "../include/storage_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void check_disk_name(const char *blkdev, const char *expected) {
  char buf[64];
  int rc = storage_extract_disk_name(blkdev, buf, sizeof(buf));

  CHECK(rc == 0, blkdev);
  CHECK(rc == 0 && strcmp(buf, expected) == 0, expected);
}

int main(void) {
  check_disk_name("/dev/sdb", "sdb");
  check_disk_name("/dev/sdb1", "sdb");
  check_disk_name("/dev/mmcblk0p1", "mmcblk0");
  check_disk_name("/dev/nvme0n1p1", "nvme0n1");

  if (failures != 0) {
    return EXIT_FAILURE;
  }

  printf("test_storage: ok\n");
  return EXIT_SUCCESS;
}
