/* zeroconf_host.h -- what zeroconf.c (the PortMaster port's s3e_zeroconf.c)
 * expects from its host, provided for Horizon.
 *
 * The engine was written for a loader where guest and host share an address
 * space, so it calls the game's callbacks directly with native pointers. Here
 * the "callbacks" it is given are host trampolines in net.c that copy each
 * result into guest memory and call the game from there; the engine itself
 * never touches guest memory. */
#ifndef ZEROCONF_HOST_H
#define ZEROCONF_HOST_H

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define S3E_SOFTFP

typedef int32_t (*s3e_zeroconf_callback_fn)(void *search, void *system_data,
                                            void *user_data);

/* Host-side layouts. net.c rebuilds them at the guest's offsets
 * (found: name 0x08, port 0x18, TXT 0x1c, IPv4 0x20, size 0x38;
 *  TXT update: count 0x06, records 0x08, size 0x0c). */
struct s3e_zeroconf_found_data {
    void *service_id;
    uint32_t reserved_04;
    const char *name;
    const char *service_type;
    const char *domain;
    const char *host;
    uint16_t port;
    uint16_t txt_count;
    const char **txt_records;
    uint32_t ipv4_address;
    uint8_t reserved_24[0x14];
};

struct s3e_zeroconf_txt_update_data {
    void *service_id;
    uint16_t reserved_04;
    uint16_t txt_count;
    const char **txt_records;
};

void *s3eZeroConfStartSearch(const char *service_type, const char *domain,
                             s3e_zeroconf_callback_fn found_callback,
                             s3e_zeroconf_callback_fn update_callback,
                             s3e_zeroconf_callback_fn lost_callback, void *user_data);
void s3eZeroConfStopSearch(void *search);
void *s3eZeroConfPublish(uint16_t port, const char *name, const char *service_type,
                         const char *domain, uint16_t txt_count, const char **txt_records);
int32_t s3eZeroConfUpdateTxtRecord(void *service, uint16_t txt_count,
                                   const char **txt_records);
int32_t s3eZeroConfUnpublish(void *service);

void s3e_zero_conf_pump(void);
void s3e_zero_conf_shutdown(void);

/* Horizon has no hostname or process ID worth advertising; the engine only
 * uses them to make its mDNS host label unique on the network. */
int zc_hostname(char *out, size_t len);
#define gethostname(out, len) zc_hostname((out), (len))
#define getpid() zc_pid()
int zc_pid(void);

#endif /* ZEROCONF_HOST_H */
