/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/random/random.h>
#include <errno.h>
#include <string.h>
#include "modem.h"
static struct nvs_fs fs;
static uint8_t pins[16][16];
static unsigned pin_count;
int settings_read(void *ctx,unsigned slot,uint8_t *data,size_t size) {
    (void)ctx; if(slot>2) return -EINVAL;
    return (int)nvs_read(&fs,(uint16_t)(0x10+slot),data,size);
}
bool settings_write(void *ctx,unsigned slot,const uint8_t *data,size_t size) {
    (void)ctx; if(slot>2) return false;
    ssize_t n=nvs_write(&fs,(uint16_t)(0x10+slot),data,size);
    return n==0 || n==(ssize_t)size;
}
bool identity_lookup(void *ctx,unsigned index,uint8_t id[16]) {
    (void)ctx;
    if(!index || index>pin_count) return false;
    memcpy(id,pins[index-1],16); return true;
}
bool identity_trust(void *ctx,const uint8_t id[16]) {
    (void)ctx;
    for(unsigned i=0;i<pin_count;i++)
        if(zm_phone(pins[i])==zm_phone(id)) return !memcmp(pins[i],id,16);
    if(pin_count==16 || nvs_write(&fs,(uint16_t)(0x100+pin_count),id,16)!=16) return false;
    uint8_t check[16];
    if(nvs_read(&fs,(uint16_t)(0x100+pin_count),check,16)!=16 || memcmp(check,id,16)) return false;
    memcpy(pins[pin_count++],id,16); return true;
}
int identity_load(uint8_t key[32]) {
    struct flash_pages_info info;
    fs.flash_device=FIXED_PARTITION_DEVICE(storage_partition);
    fs.offset=FIXED_PARTITION_OFFSET(storage_partition);
    if(!device_is_ready(fs.flash_device) || flash_get_page_info_by_offs(fs.flash_device,fs.offset,&info)) return -EIO;
    fs.sector_size=info.size;
    fs.sector_count=FIXED_PARTITION_SIZE(storage_partition)/info.size;
    if(fs.sector_count<2) return -EINVAL;
    int r=nvs_mount(&fs); if(r) return r;
    for(pin_count=0;pin_count<16;pin_count++) {
        r=nvs_read(&fs,(uint16_t)(0x100+pin_count),pins[pin_count],16);
        if(r==-ENOENT) break;
        if(r!=16) return -EIO;
    }
    r=nvs_read(&fs,1,key,32);
    if(r==32) return 0;
    if(r!=-ENOENT) return -EIO;
    if(sys_csrand_get(key,32)) return -EIO;
    if(nvs_write(&fs,1,key,32)!=32) return -EIO;
    uint8_t check[32];
    return nvs_read(&fs,1,check,32)==32 && !memcmp(key,check,32)?0:-EIO;
}
