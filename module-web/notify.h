#ifndef QDC_NOTIFY_H
#define QDC_NOTIFY_H
#include <stdint.h>
typedef struct { unsigned enabled,calls,sms; uint64_t revision; char key[129]; } QdcNotify;
int qdc_notify_read(QdcNotify *cfg);
int qdc_notify_write(const QdcNotify *cfg);
/* kind = call/sms/test, fixed official endpoint; secrets travel only in stdin. */
int qdc_notify_send(const char *kind);
#endif
