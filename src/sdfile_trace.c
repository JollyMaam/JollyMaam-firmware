#include "system/includes.h"

#include "control.h"
#include "rom.h"

struct device;
struct imount;

extern int __real_norflash_open(const char *name, struct device **device,
                                void *arg);
extern int __real_norflash_ioctl(struct device *device, u32 command, u32 arg);
extern int __real_norflash_origin_read(void *buffer, u32 address, u32 length);
extern struct imount *__real_mount(const char *device_name, const char *path,
                                  const char *filesystem, int cache_count,
                                  void *device_arg);

static void trace_color(u8 red, u8 green, u8 blue)
{
    cyclone2_led_set_all(red, green, blue);
    rom_mdelay(75U);
}

int __wrap_norflash_open(const char *name, struct device **device, void *arg)
{
    int result;

    trace_color(8U, 0U, 0U);       /* red: entering flash open */
    result = __real_norflash_open(name, device, arg);
    trace_color(8U, 8U, 0U);       /* yellow: flash open returned */
    return result;
}

int __wrap_norflash_ioctl(struct device *device, u32 command, u32 arg)
{
    int result = __real_norflash_ioctl(device, command, arg);

    trace_color(0U, 8U, 0U);       /* green: a flash ioctl returned */
    return result;
}

int __wrap_norflash_origin_read(void *buffer, u32 address, u32 length)
{
    int result = __real_norflash_origin_read(buffer, address, length);

    trace_color(0U, 0U, 8U);       /* blue: trailer read returned */
    return result;
}

struct imount *__wrap_mount(const char *device_name, const char *path,
                            const char *filesystem, int cache_count,
                            void *device_arg)
{
    struct imount *result;

    trace_color(8U, 0U, 8U);       /* magenta: entering VFS mount */
    result = __real_mount(device_name, path, filesystem, cache_count,
                          device_arg);
    trace_color(8U, 8U, 8U);       /* white: VFS mount returned */
    return result;
}
