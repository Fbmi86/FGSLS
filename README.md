# مستندات جامع سیستم‌عامل FGSLS
## (Fast Granular Secure Lock-free System)

---

## فهرست مطالب
1. [مقدمه و فلسفه طراحی](#philosophy)
2. [معماری کلی](#architecture)
3. [سیستم ذخیره‌سازی (Storage Layer)](#storage)
4. [مدیریت حافظه (Memory Management)](#memory)
5. [هم‌زمانی و قفل‌گذاری (Concurrency)](#concurrency)
6. [امنیت (Security Layer)](#security)
7. [سازگاری و تعامل (Compatibility)](#compatibility)
8. [عملکرد و بهینه‌سازی (Performance)](#performance)
9. [نقشه راه توسعه (Roadmap)](#roadmap)
10. [چالش‌ها و راه‌حل‌ها](#challenges)

---

<a name="philosophy"></a>
## ۱. مقدمه و فلسفه طراحی

### ۱.۱ چرا FGSLS؟
سیستم‌عامل‌های مدرن (Linux, Windows, macOS) بر پایه معماری‌های دهه ۱۹۷۰-۱۹۸۰ ساخته شده‌اند که با چالش‌های امروز سازگار نیستند:

- **Latency غیرقابل پیش‌بینی:** spinlock، page fault، و fsck باعث jitter می‌شوند
- **Fragmentation مزمن:** buddy allocator و inode-based FS پس از مدتی کُند می‌شوند
- **امنیت واکنشی:** آنتی‌ویروس‌ها پس از اجرا بدافزار را شناسایی می‌کنند (خیلی دیر است!)
- **پیچیدگی غیرضروری:** syscall، VFS، driver model همه لایه‌های اضافی هستند

### ۱.۲ اصول بنیادین FGSLS

#### **Quantum-First Design** ⏱️
همه چیز در کوانتوم‌های ۵۰ میکروثانیه انجام می‌شود:
- هر task دقیقاً ۵۰µs زمان CPU می‌گیرد
- هر عملیات I/O در بلوک‌های ۵۰µs شکسته می‌شود
- Scheduler هر ۵۰µs یکبار تصمیم می‌گیرد
- **مزیت:** latency قابل پیش‌بینی برای Real-Time Systems

#### **Granular Everything** 🧩
به جای ساختارهای بزرگ و یکپارچه، همه چیز به واحدهای کوچک تقسیم می‌شود:
- حافظه: Cluster ۶۴KB (نه صفحه ۴KB)
- ذخیره‌سازی: Box (۶۴KB-۲GB) یا Basket (۴KB)
- Journal: هر واحد Journal مستقل دارد
- **مزیت:** fragmentation کمتر، بازیابی سریع‌تر

#### **Security by Design** 🛡️
امنیت از ابتدا در معماری تعبیه شده، نه به‌عنوان لایه اضافی:
- SANT: بازرسی فعال قبل از اجرا
- VSub: جداسازی فضای کاری از داده‌های اصلی
- Hidden Journal: لاگ امنیتی مخفی
- **مزیت:** حمله قبل از اجرا خنثی می‌شود

#### **Lock-Free by Default** 🔓
بدون spinlock، بدون mutex، فقط:
- Atomic Operations (CAS, FAA)
- Versioned Pointers
- Lock-Free Ring Buffers
- **مزیت:** deadlock غیرممکن، scalability بهتر

#### **No Legacy Baggage** 🚫
حذف کامل:
- ❌ POSIX syscalls (fork, exec, read, write...)
- ❌ inode
- ❌ VFS layer
- ❌ /dev, /proc, /sys
- ❌ signals
- **مزیت:** کد ساده‌تر، سریع‌تر، امن‌تر

---

<a name="architecture"></a>
## ۲. معماری کلی

### ۲.۱ لایه‌های اصلی (از پایین به بالا)

```
┌─────────────────────────────────────────┐
│   Application Layer (Pure FGSLS Apps)  │
├─────────────────────────────────────────┤
│   Compatibility Layer (NSH/Sorceress)  │  ← اختیاری
├─────────────────────────────────────────┤
│   Security Layer (SANT)                │
├─────────────────────────────────────────┤
│   Task Manager (Quantum Scheduler)     │
├─────────────────────────────────────────┤
│   Memory Manager (ClusterArena + VSub) │
├─────────────────────────────────────────┤
│   Storage Layer (Warehouse Model)      │
├─────────────────────────────────────────┤
│   Hardware Abstraction (PIU)           │
├─────────────────────────────────────────┤
│   Bootloader + Firmware Interface      │
└─────────────────────────────────────────┘
```

### ۲.۲ جریان اجرا (Execution Flow)

```
Boot → PIU Init → ClusterArena Setup → 
Warehouse Mount → SANT Warmup → 
Quantum Scheduler Start → Applications
```

### ۲.۳ ساختار داده‌های کلیدی

#### **LTI (Label-Tag-ID)** 🏷️
متادیتای ۳۲ بایتی برای هر شیء:

```
Offset  | Size | Field              | Description
--------|------|--------------------|-----------------------
0x00    | 8B   | Label              | شناسه منحصربه‌فرد شیء
0x08    | 8B   | Tag                | دسته‌بندی/نوع
0x10    | 8B   | ID                 | آدرس فیزیکی/منطقی
0x18    | 2B   | Flags              | فشردگی، مالکیت، قفل
0x1A    | 2B   | Checksum (CRC16)   | یکپارچگی
0x1C    | 4B   | Version            | برای Versioned Pointers
```

**مزایا:**
- جستجو در O(1) با hash(Label+Tag)
- قرارگیری خودکار در Cache (سخت‌افزار LTI-aware)
- امکان versioning برای lock-free updates

#### **Quantum Descriptor** ⏰
هر task با این ساختار توصیف می‌شود:

```c
struct QuantumDesc {
    u64 id;                  // شناسه منحصربه‌فرد
    u64 start_tick;          // تیک شروع
    u8  priority;            // 0-255 (255=highest)
    u8  state;               // READY/RUNNING/BLOCKED
    u16 cpu_affinity;        // ماسک هسته‌های مجاز
    u32 remaining_cycles;    // چرخه CPU باقی‌مانده
    void* context;           // رجیسترها + stack
    QuantumDesc* next;       // لیست پیوندی
};
```

---

<a name="storage"></a>
## ۳. سیستم ذخیره‌سازی (Warehouse Model)

### ۳.۱ سلسله‌مراتب

```
Warehouse (کل دیسک)
    ├─ Shelf 1 (پارتیشن منطقی ∞)
    │   ├─ Box 1 (۶۴KB - ۲GB، فشرده)
    │   ├─ Box 2
    │   └─ Basket 1 (۴KB، فایل‌های کوچک)
    └─ Shelf 2
        └─ ...
```

### ۳.۲ Shelf (قفسه)

**ویژگی‌ها:**
- هر Shelf یک owner دارد (User/Admin/System)
- ACL ساده: Read/Write/Execute
- بدون محدودیت تعداد یا اندازه
- هر Shelf یک Journal مستقل دارد

**ساختار روی دیسک:**
```
Offset  | Content
--------|---------------------------
0x00    | Shelf Header (256B)
0x100   | Allocation Bitmap
0x1000  | Journal Area (8MB)
0x810000| Data Area (باقی‌مانده)
```

### ۳.۳ Box (جعبه)

**برای فایل‌های ۶۴KB تا ۲GB**

**ویژگی‌ها:**
- کل محتوا یکجا فشرده می‌شود (LZ4 یا Zstd)
- حداکثر ۳۲,۷۶۸ Cluster (۲GB / ۶۴KB)
- LTI در header ذخیره می‌شود

**نکته مهم:** اگر فایل >۲GB باشد، به چند Box موازی تقسیم می‌شود:
```
File 5GB → Box1 (2GB) + Box2 (2GB) + Box3 (1GB)
```

**ساختار:**
```
┌──────────────┐
│ Box Header   │ ← LTI + Compression Type
├──────────────┤
│ Compressed   │
│ Data         │
│ (LZ4/Zstd)   │
└──────────────┘
```

### ۳.۴ Basket (سبد)

**برای فایل‌های <۶۴KB**

چندین فایل کوچک در یک Cluster ۴KB جمع می‌شوند:
```
┌────────┬────────┬────────┬────────┐
│ File A │ File B │ File C │ Free   │
│ 1KB    │ 2KB    │ 500B   │ 500B   │
└────────┴────────┴────────┴────────┘
```

**مزایا:**
- هدررفت کمتر برای فایل‌های کوچک
- کاهش fragmentation داخلی
- سرعت بیشتر برای read/write فایل‌های کوچک

### ۳.۵ الگوریتم تصمیم‌گیری ذخیره‌سازی

```python
def storage_decision(file_size):
    if file_size < 64KB:
        return "Basket"
    elif 64KB <= file_size <= 2GB:
        return "Single Box"
    else:  # > 2GB
        num_boxes = ceil(file_size / 2GB)
        return f"Parallel Boxes (×{num_boxes})"
```

### ۳.۶ Journaling (لاگ‌گذاری)

#### **Two-Level Journal:**

**۱. Journal محلی (Per-Shelf):**
- هر Shelf یک حلقه ۸MB دارد
- هر ورودی ۶۴B است
- Write-Ahead Log برای Box/Basket

**۲. Journal مرکزی:**
- هماهنگ‌کننده بین Shelf‌ها
- Roll-Forward Points ذخیره می‌شود
- پس از هر ۱۰۰ عملیات، یک checkpoint

**۳. SANT Hidden Journal:**
- فقط خواندنی برای سیستم
- لاگ تحریک‌ها و قرنطینه‌ها
- رمزنگاری شده با کلید kernel

#### **Roll-Forward Cache (RFC):**
هر هسته CPU یک بافر ۴KB محلی دارد:
```
CPU Core 1 → RFC Buffer → Atomic Write → Journal
CPU Core 2 → RFC Buffer → Atomic Write → Journal
```

**مزیت:** کاهش contention روی Journal

---

<a name="memory"></a>
## ۴. مدیریت حافظه

### ۴.۱ ClusterArena (جایگزین Buddy/Slab)

**اصول:**
- واحد تخصیص: یک Cluster = ۶۴KB (ثابت)
- **بدون ادغام (no coalescing)**
- **بدون شکست (no splitting)**

**ساختار:**
```
ClusterArena
    ├─ Free List (لیست Cluster های آزاد)
    ├─ Active List (در حال استفاده)
    └─ Garbage List (منتظر آزادسازی)
```

**State Machine:**
```
Free → Active → Garbage → Free
```

**تخصیص:**
```c
Cluster* allocate_cluster() {
    // O(1) - فقط pop از Free List
    Cluster* c = pop(free_list);
    c->state = ACTIVE;
    push(active_list, c);
    return c;
}
```

**آزادسازی:**
```c
void free_cluster(Cluster* c) {
    c->state = GARBAGE;
    push(garbage_list, c);
    // ZHT بعداً پاکسازی می‌کند
}
```

### ۴.۲ VSub (Visual Subtraction)

دو فضای حافظه موازی:

#### **visual_space (حافظه دائمی):**
- داده‌های اصلی اینجا هستند
- فقط kernel دسترسی مستقیم دارد
- محافظت شده با Hardware Memory Protection

#### **working_space (حافظه موقت):**
- برنامه‌ها اینجا کار می‌کنند
- نسخه کپی‌شده از visual_space
- پس از پایان task، **کاملاً پاک می‌شود**

**جریان کار:**
```
1. App درخواست داده می‌کند
2. Kernel نسخه کپی می‌کند: visual → working
3. App روی working_space کار می‌کند
4. App عملیات را commit می‌کند
5. Kernel تغییرات را اعمال می‌کند: working → visual
6. working_space پاک می‌شود (memset 0)
```

**مزایا:**
- داده‌های اصلی هرگز فاسد نمی‌شوند
- امنیت: اگر App کرش کند، visual_space سالم است
- Rollback ساده: فقط working_space را دور بریز

**Trade-off:**
- استفاده از RAM تا ۲× بیشتر (اما امنیت ارزشش را دارد)

### ۴.۳ ZHT (Zombie Hunter Thread)

وظیفه: پاکسازی Cluster‌های orphaned

**الگوریتم:**
```python
def zht_worker():
    while True:
        sleep(2 * checkpoint_interval)  # ۲ checkpoint صبر کن
        
        for cluster in garbage_list:
            if cluster.reference_count == 0:
                if cluster.checkpoint_age >= 2:
                    # امن است، آزاد کن
                    move(cluster, free_list)
                    cluster.state = FREE
```

**چرا ۲ checkpoint؟**
اگر یک rollback رخ دهد، ممکن است به Cluster قدیمی نیاز باشیم. بنابراین ۲ نسخه نگه می‌داریم.

---

<a name="concurrency"></a>
## ۵. هم‌زمانی و قفل‌گذاری

### ۵.۱ Quantum Lock (حالت پیش‌فرض)

**بدون spinlock، بدون mutex!**

به جای lock، از **Versioned Pointers** استفاده می‌کنیم:

```c
struct VersionedPtr {
    void* ptr;      // آدرس داده
    u32 version;    // شماره نسخه
    u32 _padding;
};

// Atomic Read
VersionedPtr read_versioned(VersionedPtr* vp) {
    VersionedPtr snapshot;
    do {
        snapshot = atomic_load(vp);
    } while (snapshot.version & 1);  // فرد = در حال نوشتن
    return snapshot;
}

// Atomic Write
bool write_versioned(VersionedPtr* vp, void* new_ptr) {
    VersionedPtr old, new_vp;
    do {
        old = atomic_load(vp);
        new_vp.ptr = new_ptr;
        new_vp.version = old.version + 2;  // +2 تا زوج بماند
    } while (!CAS(vp, old, new_vp));
    return true;
}
```

**مزایا:**
- خوانده‌ها هرگز block نمی‌شوند
- نویسنده‌ها فقط یک CAS می‌زنند
- deadlock غیرممکن است

### ۵.۲ Lock-Free Ring Buffer

برای ارتباط بین CPU Core‌ها:

```c
struct LFRing {
    u64 head;           // atomic
    u64 tail;           // atomic
    void* buffer[SIZE];
};

bool enqueue(LFRing* ring, void* item) {
    u64 h = atomic_load(&ring->head);
    u64 next = (h + 1) % SIZE;
    if (next == atomic_load(&ring->tail))
        return false;  // پر است
    
    ring->buffer[h] = item;
    atomic_store(&ring->head, next);
    return true;
}
```

### ۵.۳ Atomic Spinlock (حالت Fallback)

برای سخت‌افزارهایی که Atomic Operations پیشرفته ندارند:

```c
typedef struct {
    atomic_int lock;
} AtomicSpinlock;

void acquire(AtomicSpinlock* s) {
    while (atomic_exchange(&s->lock, 1) == 1) {
        cpu_relax();  // PAUSE instruction
    }
}

void release(AtomicSpinlock* s) {
    atomic_store(&s->lock, 0);
}
```

**کاربرد:** فقط برای critical section‌های <۱۰µs

### ۵.۴ Quantum Scheduler

**الگوریتم:**

```python
def quantum_scheduler():
    current_tick = 0
    
    while True:
        # هر ۵۰µs یکبار
        wait_until(current_tick * 50)
        
        # انتخاب task با بالاترین priority
        task = select_highest_priority()
        
        if task:
            # اجرای دقیقاً ۵۰µs
            run_task(task, quantum=50)
            
            # Roll-Forward Point
            if current_tick % 20 == 0:  # هر ۱ms
                checkpoint()
        
        current_tick += 1
```

**Priority Classes:**
- **255-192:** Real-Time (hard deadline)
- **191-128:** Interactive (UI, games)
- **127-64:** Normal (برنامه‌های معمولی)
- **63-0:** Background (backup، indexing)

---

<a name="security"></a>
## ۶. امنیت (SANT)

### ۶.۱ Security Alert and Neutralization Tool

**فلسفه:** بدافزار را قبل از اجرا خنثی کن، نه بعد از آن!

### ۶.۲ فرآیند عملیات SANT

#### **مرحله ۱: نظارت (Monitoring)**
```
SANT → مانیتور پروسه‌های فعال
      ├─ CPU Usage بررسی کن
      ├─ Memory Access Pattern تحلیل کن
      └─ System Call Pattern شناسایی کن
```

**نشانه‌های مشکوک:**
- پروسه در حالت IDLE ولی CPU می‌خورد
- دسترسی‌های تصادفی به حافظه
- تلاش برای نوشتن در Kernel Space

#### **مرحله ۲: قرنطینه (Quarantine)**
```c
void sant_quarantine(Process* proc) {
    // ۱. تعلیق فوری
    proc->state = SUSPENDED;
    
    // ۲. انتقال به فضای ایزوله
    move_to_sandbox(proc);
    
    // ۳. ثبت در Hidden Journal
    log_to_hidden_journal(proc, "QUARANTINED");
}
```

#### **مرحله ۳: تحریک (Deception)**

SANT خودش را جای سیستم‌عامل واقعی می‌گذارد:

```python
def deception_test(process):
    # ایجاد محیط جعلی
    fake_os = create_fake_environment()
    
    # اجرای پروسه در محیط جعلی
    fake_os.run(process)
    
    # مانیتور رفتار
    behaviors = monitor_behaviors(process, duration=10)
    
    # تحلیل
    if is_malicious(behaviors):
        terminate(process)
        alert_admin()
    else:
        release_from_quarantine(process)
```

**تحریک‌های معمول:**
- Syscall‌های جعلی که "موفق" برمی‌گردند ولی کاری نمی‌کنند
- فایل‌های دمی (honeypot)
- شبکه جعلی که به هیچ جا متصل نیست

**الگوهای مخرب:**
- تلاش برای رمزنگاری فایل‌ها (Ransomware)
- اتصال به C&C Server (Botnet)
- خواندن کلیدهای رمزنگاری (Keylogger)
- تزریق کد به پروسه‌های دیگر

### ۶.۳ SANT Hidden Journal

```
Offset  | Field              | Size
--------|--------------------|---------
0x00    | Event Type         | 1B
0x01    | Timestamp          | 8B
0x09    | Process ID         | 8B
0x11    | Behavior Hash      | 32B
0x31    | Signature          | 64B (Ed25519)
```

**رمزنگاری:**
- هر ورودی با کلید kernel امضا می‌شود
- فقط در Safe Mode قابل خواندن
- دستکاری = بوت نمی‌شود

---

<a name="compatibility"></a>
## ۷. سازگاری و تعامل

### ۷.۱ NSH (Native-to-Standard Handler)

**هدف:** ترجمه on-the-fly به ext4/NTFS

```
FGSLS Storage → NSH → Virtual ext4/NTFS Layer → OS دیگر
```

**مثال:**
```
Linux می‌خواهد "/home/user/file.txt" را بخواند:
1. NSH درخواست را دریافت می‌کند
2. مسیر را به Shelf/Box ترجمه می‌کند
3. Box را uncompress می‌کند
4. داده را در فرمت ext4 برمی‌گرداند
```

### ۷.۲ Sorceress (Module Barrandaz)

**مشکل:** وقتی دیسک FGSLS را در Windows/Linux می‌زنید، "Disk not formatted" می‌گوید.

**راه‌حل:**
```c
// جلوی فرمت را بگیر!
void sorceress_intercept() {
    if (detect_format_request()) {
        show_popup("این دیسک FGSLS است. از NSH استفاده کنید.");
        mount_via_nsh();
    }
}
```

**ویژگی‌ها:**
- شناسایی خودکار سیستم‌عامل میزبان
- نصب درایور NSH
- ایجاد mount point مجازی

### ۷.۳ Transporter

**مدیریت Copy/Move با قابلیت Resume**

```python
def transport_file(src, dst):
    # تقسیم به Quantum های ۶۴KB
    chunks = split_to_chunks(src, size=64KB)
    
    for i, chunk in enumerate(chunks):
        # لاگ کردن پیشرفت
        log_transport(src, dst, chunk_id=i, state="COPYING")
        
        # کپی
        write_chunk(dst, chunk)
        
        # تأیید
        log_transport(src, dst, chunk_id=i, state="DONE")
    
    # پاکسازی لاگ
    finalize_transport(src, dst)
```

**قطع برق؟**
```python
def resume_transport():
    logs = read_incomplete_transports()
    
    for log in logs:
        last_chunk = log.last_completed_chunk
        resume_from_chunk(log.src, log.dst, last_chunk + 1)
```

---

<a name="performance"></a>
## ۸. عملکرد و بهینه‌سازی

### ۸.۱ PIU (Performance Intelligence Unit)

**مانیتور سلامت و تخصیص منابع:**

```python
def piu_allocator():
    while True:
        # هر ۱ms یکبار
        stats = collect_hardware_stats()
        
        # تحلیل
        bottleneck = detect_bottleneck(stats)
        
        if bottleneck == "CPU":
            boost_cpu_frequency()
        elif bottleneck == "RAM":
            trigger_zht_aggressive()  # آزادسازی سریع‌تر
        elif bottleneck == "DISK":
            enable_aggressive_caching()
```

**Quantum-Level Priority:**
```
Task A (RT, priority=255) → CPU Core 0 (dedicated)
Task B (Interactive, priority=150) → CPU Core 1
Task C (Background, priority=30) → CPU Core 2 (shared)
```

### ۸.۲ Caching Strategy

#### **L1: LRU Cache در RAM**
- ۱۰۲۴ اخیرترین Box Header
- Hit Rate معمولاً >۹۰٪

#### **L2: Prediction Cache**
```python
def predict_next_access(current_file):
    # الگوریتم یادگیری ساده
    history = get_access_history(current_file)
    
    # فایل‌های مرتبط
    related = find_related_files(history)
    
    # پیش‌بارگذاری
    for f in related[:5]:
        preload_to_cache(f)
```

### ۸.۳ Compression Benchmark

| الگوریتم | سرعت فشردن | سرعت باز کردن | نسبت فشردگی | کاربرد |
|----------|------------|--------------|-------------|--------|
| LZ4      | ~500 MB/s  | ~2000 MB/s   | ۲-۳×        | پیش‌فرض |
| Zstd     | ~200 MB/s  | ~600 MB/s    | ۳-۵×        | آرشیو |
| بدون فشردگی | ∞      | ∞            | ۱×          | فایل‌های کوچک |

**تصمیم‌گیری خودکار:**
```python
def choose_compression(file_size, file_type):
    if file_size < 64KB:
        return None  # Basket بدون فشردگی
    
    if file_type in ["jpg", "mp4", "zip"]:
        return None  # قبلاً فشرده است
    
    if file_size < 10MB:
        return "LZ4"  # سریع
    else:
        return "Zstd"  # فشردگی بهتر
```

---

<a name="roa
