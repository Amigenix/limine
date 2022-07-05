#include <stddef.h>
#include <stdint.h>
#include <fs/file.h>
#include <fs/echfs.h>
#include <fs/ext2.h>
#include <fs/fat32.h>
#include <fs/iso9660.h>
#include <fs/ntfs.h>
#include <lib/print.h>
#include <lib/blib.h>
#include <mm/pmm.h>
#include <lib/part.h>
#include <lib/libc.h>
#include <pxe/tftp.h>

char *fs_get_label(struct volume *part) {
    char *ret;

    if ((ret = fat32_get_label(part)) != NULL) {
        return ret;
    }
    if ((ret = ext2_get_label(part)) != NULL) {
        return ret;
    }

    return NULL;
}

bool fs_get_guid(struct guid *guid, struct volume *part) {
    if (echfs_get_guid(guid, part) == true) {
        return true;
    }
    if (ext2_get_guid(guid, part) == true) {
        return true;
    }

    return false;
}

struct file_handle *fopen(struct volume *part, const char *filename) {
    if (strlen(filename) + 2 > PATH_MAX) {
        panic(true, "fopen: Path too long");
    }

    struct file_handle *ret;

#if bios == 1
    if (part->pxe) {
        if ((ret = tftp_open(0, 69, filename)) == NULL) {
            return NULL;
        }
        return ret;
    }
#endif

    if ((ret = echfs_open(part, filename)) != NULL) {
        return ret;
    }
    if ((ret = ext2_open(part, filename)) != NULL) {
        return ret;
    }
    if ((ret = fat32_open(part, filename)) != NULL) {
        return ret;
    }
    if ((ret = iso9660_open(part, filename)) != NULL) {
        return ret;
    }
    if ((ret = ntfs_open(part, filename)) != NULL) {
        return ret;
    }

    return NULL;
}

void fclose(struct file_handle *fd) {
    if (fd->is_memfile) {
        if (fd->readall == false) {
            pmm_free(fd->fd, fd->size);
        }
        pmm_free(fd, sizeof(struct file_handle));
    } else {
        fd->close(fd);
    }
}

void fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count) {
    if (fd->is_memfile) {
        memcpy(buf, fd->fd + loc, count);
    } else {
        fd->read(fd, buf, loc, count);
    }
}

void *freadall(struct file_handle *fd, uint32_t type) {
    if (fd->is_memfile) {
        if (fd->readall) {
            return fd->fd;
        }
        memmap_alloc_range((uint64_t)(size_t)fd->fd, ALIGN_UP(fd->size, 4096), type, false, true, false, false);
        fd->readall = true;
        return fd->fd;
    } else {
        void *ret = ext_mem_alloc_type(fd->size, type);
        fd->read(fd, ret, 0, fd->size);
        return ret;
    }
}
