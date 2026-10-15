"""No modem operations: persistent history, warm/cold restart and clear semantics."""
import ctypes as C, os, pathlib, subprocess, tempfile, time, multiprocessing, signal
R=pathlib.Path(__file__).resolve().parent
subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-DHISTORY_TEST','-shared','-fPIC',str(R/'call-history.c'),'-o',str(R/'build/history-test.dylib')],check=True)
class Call(C.Structure):_fields_=[('id',C.c_uint),('state',C.c_uint),('direction',C.c_uint),('number',C.c_char*81)]
class Row(C.Structure):_fields_=[('id',C.c_char*33),('boot',C.c_char*37),('number',C.c_char*81)]+[(n,C.c_uint) for n in ['call_id','direction','status','hidden','imported']]+[(n,C.c_uint64) for n in ['started','ended','answered','answer_tick','duration']]
class History(C.Structure):_fields_=[('count',C.c_uint),('cleared',C.c_uint64),('rows',Row*500)]
lib=C.CDLL(str(R/'build/history-test.dylib'))
for n in ['snapshot','read','clear','import']:getattr(lib,'qdc_history_'+n).restype=C.c_int
lib.qdc_history_snapshot.argtypes=[C.c_char_p,C.c_char_p,C.POINTER(Call),C.c_size_t]
lib.qdc_history_import.argtypes=[C.c_char_p,C.c_char_p,C.POINTER(Row),C.c_size_t]
lib.qdc_history_read.argtypes=[C.c_char_p,C.c_char_p,C.POINTER(History)]
lib.qdc_history_clear.argtypes=[C.c_char_p,C.c_char_p]
with tempfile.TemporaryDirectory(prefix='qdc-history-') as folder:
 root=pathlib.Path(folder);private=root/'private';run=root/'run';private.mkdir(mode=0o700);run.mkdir(mode=0o700);args=(str(private).encode(),str(run).encode())
 os.environ['QDC_HISTORY_BOOT']='00000000-0000-0000-0000-000000000001'
 def snap(*calls):return lib.qdc_history_snapshot(*args,(Call*len(calls))(*calls),len(calls))
 def read():
  h=History();assert lib.qdc_history_read(*args,C.byref(h));return h
 incoming=Call(4,2,2,b'10086');assert snap(incoming)
 first=read().rows[0].id;disk=private/'call-history';unchanged=disk.read_bytes()
 assert snap(incoming) and disk.read_bytes()==unchanged,'no ongoing periodic flash writes'
 # Fresh caller process / warm watcher restart shares the same active record.
 assert read().count==1 and read().rows[0].id==first
 incoming.state=3;assert snap(incoming);assert read().rows[0].answered
 assert snap();ended=read().rows[0];assert ended.status==1 and ended.ended>=ended.started
 assert snap(incoming) and read().count==2,'reused modem call ID must get a new record'
 assert lib.qdc_history_clear(*args);assert not [r for r in read().rows[:read().count] if not r.hidden]
 assert snap(incoming) and read().count==1 and read().rows[0].hidden,'clear must not resurrect current call'
 assert snap();assert read().count==0
 assert snap(Call(5,1,1,b'10010'));old=read().rows[0].id
 os.environ['QDC_HISTORY_BOOT']='00000000-0000-0000-0000-000000000002'
 h=read();assert h.count==1 and h.rows[0].status==2 and h.rows[0].ended==0
 assert snap(Call(5,1,1,b'10010'));h=read();assert h.count==2 and h.rows[1].id!=old
 assert snap()
 now=int(time.time()*1000);legacy=Row();legacy.direction=2;legacy.started=now-1;legacy.ended=now;legacy.number=b'10000'
 assert lib.qdc_history_import(*args,C.byref(legacy),1);n=read().count
 assert lib.qdc_history_import(*args,C.byref(legacy),1) and read().count==n
 assert lib.qdc_history_clear(*args);assert lib.qdc_history_import(*args,C.byref(legacy),1) and read().count==0,'clear cutoff blocks old browser reimports'
 legacy.started=now+100000;legacy.ended=legacy.started
 for i in range(520):
  legacy.started+=1;legacy.ended+=1;assert lib.qdc_history_import(*args,C.byref(legacy),1)
 assert read().count==500
 # Old records do not evict newer records during migration.
 before=disk.read_bytes();legacy.started=now+99999;legacy.ended=legacy.started
 assert lib.qdc_history_import(*args,C.byref(legacy),1) and disk.read_bytes()==before
 assert disk.stat().st_mode&0o777==0o600 and disk.stat().st_size<150000
 # Unsafe/corrupt state is refused, never silently replaced with an empty log.
 before=disk.read_bytes();disk.write_text('corrupt\n');assert not snap();h=History();assert not lib.qdc_history_read(*args,C.byref(h));assert disk.read_text()=='corrupt\n';disk.write_bytes(before)
 target=private/'sentinel';target.write_text('keep');target.chmod(0o600);disk.unlink();disk.symlink_to(target);assert not snap() and target.read_text()=='keep';disk.unlink();disk.write_bytes(before);disk.chmod(0o600)
 # A leftover crash temporary file never prevents the next commit.
 orphan=private/'.call-history-abcdef';orphan.write_text('partial');orphan.chmod(0o600)
 assert snap(Call(6,2,2,b'10086'));assert snap();assert not orphan.exists()
 # Kill a writer during repeated atomic commits; the committed file remains parseable.
 def writer():
  for _ in range(10000):assert snap(Call(8,1,1,b'10086'));assert snap()
 proc=multiprocessing.get_context('fork').Process(target=writer);proc.start();time.sleep(.04);os.kill(proc.pid,signal.SIGKILL);proc.join()
 assert lib.qdc_history_read(*args,C.byref(History()))
print('Call history passed: durable background snapshots, no idle writes, warm/cold restart, reused IDs, clear while active, idempotent migration, retention, private permissions, corruption/symlink refusal and killed-writer recovery.')
