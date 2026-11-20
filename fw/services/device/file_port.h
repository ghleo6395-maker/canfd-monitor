/* Device file metadata boundary shared by the native simulator and Rev D. */
#ifndef DEVICE_FILE_PORT_H
#define DEVICE_FILE_PORT_H
#include <stdint.h>

#define FILE_PORT_REC_PREFIX_MAX_BYTES 18u
#define FILE_PORT_NAME_MAX_BYTES 47u
typedef enum {
    FILE_PORT_OK=0, FILE_PORT_ERR_ARGUMENT=-1, FILE_PORT_ERR_LIMIT=-2,
    FILE_PORT_ERR_FORMAT=-3, FILE_PORT_ERR_TIMESTAMP=-4, FILE_PORT_ERR_NO_SLOT=-5,
    FILE_PORT_ERR_NOT_AVAILABLE=-6, FILE_PORT_ERR_SPACE=-7, FILE_PORT_ERR_BUSY=-8,
    FILE_PORT_ERR_EXISTS=-9, FILE_PORT_ERR_READONLY=-10, FILE_PORT_ERR_CHANGED=-11,
    FILE_PORT_ERR_NAME=-12, FILE_PORT_ERR_EXTENSION=-13, FILE_PORT_ERR_IO=-14
} file_port_result_t;

/* Metadata queries are cached and never scan/mount the card. Zero token means
 * absent or ambiguous. Tokens survive index compaction and change on replacement.
 * A record token binds the actual recording session, never an imported file. */
uint32_t file_port_token(int index);
uint32_t file_port_record_token(uint32_t session);
const char *file_port_rec_prefix_get(void);
int file_port_rec_prefix_set(const char *prefix);
/* Validate closed/writable state, token and name before committing. Never
 * overwrite another file. Failure preserves the original file and its name. */
int file_port_rename(int index,uint32_t expected_token,const char *name);

#endif
