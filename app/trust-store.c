/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "../build/ca-bundle.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int ps5_install_trust_store(void) {
    if (mkdir(PS5_AI_STATE "/certs",0700) && errno!=EEXIST) return -1;
    int parent=open(PS5_AI_STATE "/certs",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if (parent<0) return -1;
    int file=openat(parent,PS5_CA_NAME,O_RDONLY|O_NOFOLLOW);
    int result=-1;
    if (file>=0) {
        struct stat info; unsigned char buffer[4096]; size_t offset=0;
        if (fstat(file,&info) || !S_ISREG(info.st_mode) || info.st_size!=(off_t)sizeof(ps5_ca_bundle)) goto done;
        while (offset<sizeof(ps5_ca_bundle)) {
            size_t length=sizeof(ps5_ca_bundle)-offset;
            if (length>sizeof(buffer)) length=sizeof(buffer);
            ssize_t n=read(file,buffer,length);
            if (n<0 && errno==EINTR) continue;
            if (n<=0 || memcmp(buffer,ps5_ca_bundle+offset,(size_t)n)) goto done;
            offset+=(size_t)n;
        }
        result=0;
    } else if (errno==ENOENT) {
        /* Content-addressed names avoid silently replacing an unknown bundle. */
        file=openat(parent,PS5_CA_NAME,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if (file<0) goto done;
        size_t offset=0;
        while (offset<sizeof(ps5_ca_bundle)) {
            ssize_t n=write(file,ps5_ca_bundle+offset,sizeof(ps5_ca_bundle)-offset);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) goto done;
            offset+=(size_t)n;
        }
        result=fsync(file);
    }
done:
    if (file>=0 && close(file)) result=-1;
    close(parent);
    if (!result) result=setenv("SSL_CERT_FILE",PS5_AI_STATE "/certs/" PS5_CA_NAME,1);
    return result;
}
