#ifndef QDC_CALL_HISTORY_H
#define QDC_CALL_HISTORY_H
#include <stdint.h>
#include <stddef.h>
#define QDC_HISTORY_LIMIT 500
/* Status: 0 active, 1 ended, 2 interrupted (reboot), 3 imported/unknown. */
typedef struct { unsigned id,state,direction; char number[81]; } QdcHistoryCall;
typedef struct {
 char id[33],boot[37],number[81];
 unsigned call_id,direction,status,hidden,imported;
 uint64_t started,ended,answered,answer_tick,duration;
} QdcHistoryRow;
typedef struct { unsigned count; uint64_t cleared; QdcHistoryRow rows[QDC_HISTORY_LIMIT]; } QdcHistory;
int qdc_history_snapshot(const char *private_dir,const char *run,const QdcHistoryCall *calls,size_t count);
int qdc_history_read(const char *private_dir,const char *run,QdcHistory *history);
int qdc_history_clear(const char *private_dir,const char *run);
int qdc_history_import(const char *private_dir,const char *run,const QdcHistoryRow *rows,size_t count);
#endif
