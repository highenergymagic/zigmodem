/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/random/random.h>
#include <errno.h>
#include <string.h>
int identity_load(uint8_t key[32]) {
    struct nvs_fs fs={0}; struct flash_pages_info info;
    fs.flash_device=FIXED_PARTITION_DEVICE(storage_partition);
    fs.offset=FIXED_PARTITION_OFFSET(storage_partition);
    if(!device_is_ready(fs.flash_device) || flash_get_page_info_by_offs(fs.flash_device,fs.offset,&info)) return -EIO;
    fs.sector_size=info.size;
    fs.sector_count=FIXED_PARTITION_SIZE(storage_partition)/info.size;
    if(fs.sector_count<2) return -EINVAL;
    int r=nvs_mount(&fs); if(r) return r;
    r=nvs_read(&fs,1,key,32);
    if(r==32) return 0;
    if(r!=-ENOENT) return -EIO;
    if(sys_csrand_get(key,32)) return -EIO;
    if(nvs_write(&fs,1,key,32)!=32) return -EIO;
    uint8_t check[32];
    return nvs_read(&fs,1,check,32)==32 && !memcmp(key,check,32)?0:-EIO;
}
