/* SPDX-License-Identifier: GPL-3.0-or-later
 * Find this exact build in Payload Manager, then install its runtime atomically.
 * An ELF note avoids reading every candidate payload into memory or guessing
 * that another file with a similar name is the currently running build.
 */
#include "config.h"
#include "../build/terminal-build-id.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
struct image_note { uint32_t namesz, descsz, type; char name[12]; char id[32]; };
__attribute__((section(".note.ps5aicli"), used, aligned(4)))
static const struct image_note this_image = {9,32,0x50533541,"PS5AICLI",PS5_AI_BUILD_ID};
extern int ps5_set_executable_path(const char *path);
#ifndef PS5_AI_PAYLOAD_ROOT
#define PS5_AI_PAYLOAD_ROOT "/data/pldmgr/payloads"
#endif

static int read_at(int fd, void *buffer, size_t length, off_t offset) {
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = pread(fd,p,length,offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n; offset += n;
    }
    return 0;
}
/* 2 is this build; 1 is another build of this app; 0 is not an owned image. */
static int image_kind(int fd) {
    struct stat info; Elf64_Ehdr h;
    if (fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size < (off_t)sizeof(h) ||
        read_at(fd,&h,sizeof(h),0) || memcmp(h.e_ident,"\177ELF\2\1",6) ||
        h.e_type != ET_DYN || h.e_machine != EM_X86_64 || h.e_shentsize != sizeof(Elf64_Shdr) ||
        !h.e_shnum || h.e_shnum > 8192 || h.e_shoff > (uint64_t)info.st_size ||
        (uint64_t)h.e_shnum * sizeof(Elf64_Shdr) > (uint64_t)info.st_size - h.e_shoff) return 0;
    for (unsigned i=0; i<h.e_shnum; i++) {
        Elf64_Shdr section;
        if (read_at(fd,&section,sizeof(section),(off_t)(h.e_shoff+i*sizeof(section)))) return 0;
        if (section.sh_size != sizeof(this_image) || section.sh_type == SHT_NOBITS ||
            section.sh_offset > (uint64_t)info.st_size ||
            section.sh_size > (uint64_t)info.st_size - section.sh_offset) continue;
        struct image_note note;
        if (read_at(fd,&note,sizeof(note),(off_t)section.sh_offset)) return 0;
        if (!memcmp(&note,&this_image,24)) return !memcmp(note.id,this_image.id,32) ? 2 : 1;
    }
    return 0;
}
static int payload_source(void) {
    DIR *dir=opendir(PS5_AI_PAYLOAD_ROOT);
    if (!dir) return -1;
    struct dirent *entry; int found=-1;
    while ((entry=readdir(dir))) {
        const char *name=entry->d_name;
        if ((strcmp(name,"ps5-ai-cli") && strncmp(name,"ps5-ai-cli-",11)) || strlen(name)>96) continue;
        int valid=1;
        for (const char *p=name; *p; p++) if (!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-')) valid=0;
        if (!valid) continue;
        int folder=openat(dirfd(dir),name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if (folder<0) continue;
        /* Payload Manager may group updated ELFs under the app's base name,
         * or give each upload its own content-derived directory. */
        DIR *files=fdopendir(folder);
        if (!files) { close(folder); continue; }
        struct dirent *file;
        while ((file=readdir(files))) {
            const char *filename=file->d_name;
            size_t length=strlen(filename);
            int plain=!strcmp(filename,"ps5-ai-cli.elf");
            if (!plain && (length<16 || length>112 || strncmp(filename,"ps5-ai-cli-",11) ||
                strcmp(filename+length-4,".elf"))) continue;
            int safe=1;
            for (size_t i=11; !plain && i<length-4; i++)
                if (!((filename[i]>='a'&&filename[i]<='z') ||
                      (filename[i]>='0'&&filename[i]<='9') || filename[i]=='-')) safe=0;
            if (!safe) continue;
            int fd=openat(folder,filename,O_RDONLY|O_NOFOLLOW);
            if (fd<0) continue;
            if (image_kind(fd)==2) { found=fd; break; }
            close(fd);
        }
        closedir(files);
        if (found>=0) break;
    }
    closedir(dir);
    if (found<0) errno=ENOENT;
    return found;
}
int ps5_install_runtime_image(void) {
    const char *directory=PS5_AI_STATE "/runtime";
    const char *path=PS5_AI_STATE "/runtime/codex.elf";
    if (mkdir(directory,0700) && errno!=EEXIST) return -1;
    int parent=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if (parent<0) return -1;
    int existing=openat(parent,"codex.elf",O_RDONLY|O_NOFOLLOW);
    if (existing>=0) {
        int kind=image_kind(existing); close(existing);
        if (kind==2) { close(parent); return ps5_set_executable_path(path); }
        if (!kind) { close(parent); errno=EEXIST; return -1; }
    } else if (errno!=ENOENT) { close(parent); return -1; }
    int source=payload_source();
    if (source<0) { close(parent); return -1; }
    char temp[64]; snprintf(temp,sizeof(temp),".codex-%ld.tmp",(long)getpid());
    int output=openat(parent,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0700);
    if (output<0) { close(source);close(parent);return -1; }
    unsigned char buffer[65536]; int result=-1;
    for (;;) {
        ssize_t n=read(source,buffer,sizeof(buffer));
        if (n<0 && errno==EINTR) continue;
        if (n<0) break;
        if (!n) { result=fsync(output);break; }
        size_t offset=0;
        while (offset<(size_t)n) {
            ssize_t written=write(output,buffer+offset,(size_t)n-offset);
            if (written<0 && errno==EINTR) continue;
            if (written<=0) goto done;
            offset+=(size_t)written;
        }
    }
done:
    if (close(output)) result=-1;
    close(source);
    if (!result) result=renameat(parent,temp,parent,"codex.elf");
    if (!result) result=fsync(parent);
    if (result) unlinkat(parent,temp,0);
    close(parent);
    return result ? -1 : ps5_set_executable_path(path);
}
