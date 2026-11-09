#include "storage.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int locate_eboot(StStorage *paths,const char *file) {
    if(!file||strlen(file)>=sizeof(paths->eboot))return 0;
    const char *device=!strncasecmp(file,"ms0:/",5)?"ms0:":
                       !strncasecmp(file,"ef0:/",5)?"ef0:":NULL;
    if(!device)return 0;
    const char *name=strrchr(file,'/');
    if(strcasecmp(name+1,"EBOOT.PBP"))return 0;

    st_copy(paths->eboot,sizeof(paths->eboot),file);
    memcpy(paths->eboot,device,4);
    struct stat info;
    if(stat(paths->eboot,&info)||!S_ISREG(info.st_mode))return 0;

    size_t directory_length=(size_t)(name-file);
    memcpy(paths->game,paths->eboot,directory_length);
    paths->game[directory_length]=0;
    snprintf(paths->savedata,sizeof(paths->savedata),"%s/PSP/SAVEDATA/%s",device,ST_SAVE_DIRECTORY);
    int length=snprintf(paths->config,sizeof(paths->config),"%s/config.ini",paths->game);
    return length>0&&(size_t)length<sizeof(paths->config);
}

int st_storage_locate(StStorage *paths,const char *boot_file,const char *argument,const char *cwd,StError *err) {
    if(locate_eboot(paths,boot_file)||locate_eboot(paths,argument))return 1;
    if(cwd&&*cwd) {
        char file[sizeof(paths->eboot)];
        const char *separator=cwd[strlen(cwd)-1]=='/'?"":"/";
        int length=snprintf(file,sizeof(file),"%s%sEBOOT.PBP",cwd,separator);
        if(length>0&&(size_t)length<sizeof(file)&&locate_eboot(paths,file))return 1;
    }
    return st_fail(err,"Cannot locate EBOOT.PBP. Check the launch path.");
}
