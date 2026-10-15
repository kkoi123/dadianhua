static int history_compare(const void *a,const void *b){const QdcHistoryRow *x=a,*y=b;return x->started>y->started?-1:x->started<y->started?1:strcmp(x->id,y->id);}
static void history_list(void){const char *cursor=field("cursor");char *end;unsigned long offset=strtoul(*cursor?cursor:"0",&end,10);if(*end||offset>QDC_HISTORY_LIMIT){error_response(400,"通话记录分页参数无效");return;}
 char directory[768];QdcHistory *h=malloc(sizeof *h);if(!h||!path(directory,sizeof directory,root,"private")||!qdc_history_read(directory,runtime,h)){free(h);error_response(503,"通话记录暂无法读取，请稍后重试");return;}
 unsigned total=0;for(unsigned i=0;i<h->count;i++)if(!h->rows[i].hidden)h->rows[total++]=h->rows[i];qsort(h->rows,total,sizeof *h->rows,history_compare);unsigned limit=(unsigned)offset+20;if(limit>total)limit=total;
 headers(200,NULL);printf("{\"ok\":true,\"total\":%u,\"records\":[",total);int emitted=0;
 for(unsigned i=(unsigned)offset;i<limit;i++){QdcHistoryRow *r=&h->rows[i];if(emitted++)putchar(',');printf("{\"id\":");json_string(r->id);printf(",\"number\":");json_string(r->number);printf(",\"direction\":%u,\"started\":%" PRIu64 ",\"ended\":%" PRIu64 ",\"answered\":%" PRIu64 ",\"duration\":%" PRIu64 ",\"status\":%u,\"imported\":%s}",r->direction,r->started,r->ended,r->answered,r->duration,r->status,r->imported?"true":"false");}
 printf("],\"next\":");if(limit<total)printf("%u",limit);else printf("null");puts("}");free(h);
}
static void history_clear(void){char directory[768];if(!path(directory,sizeof directory,root,"private")||!qdc_history_clear(directory,runtime)){error_response(503,"通话记录清除未确认，请刷新核对");return;}headers(200,NULL);puts("{\"ok\":true}");}
static void history_import(void){/* Flat string batches keep the existing JSON body parser bounded. */
 char input[4097];snprintf(input,sizeof input,"%s",field("records"));QdcHistoryRow rows[20]={0};size_t count=0;char *saveptr=NULL,*line=strtok_r(input,"\n",&saveptr),extra;
 while(line){if(count>=20){error_response(400,"历史记录数量过多");return;}QdcHistoryRow *r=&rows[count];if(sscanf(line,"%u %" SCNu64 " %" SCNu64 " %80s %c",&r->direction,&r->started,&r->ended,r->number,&extra)!=4){error_response(400,"历史记录格式无效");return;}if(!strcmp(r->number,"-"))r->number[0]=0;count++;line=strtok_r(NULL,"\n",&saveptr);}
 if(!count){error_response(400,"没有历史记录");return;}char directory[768];if(!path(directory,sizeof directory,root,"private")||!qdc_history_import(directory,runtime,rows,count)){error_response(503,"历史记录迁移未完成，请稍后重试");return;}headers(200,NULL);puts("{\"ok\":true}");
}
