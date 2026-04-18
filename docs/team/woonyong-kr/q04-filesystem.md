# Q17. 리눅스 파일시스템 완전 해부 — VFS·ext4·페이지캐시·블록 레이어·sockfs

> CSAPP 10장 + 리눅스 커널 코드 | "모든 것은 파일" 이 실제 어떻게 구현되어 있는가 | 심화

## 질문

1. VFS 네 개의 핵심 객체 (superblock / inode / dentry / file) 는 어떤 관계이고 커널에서 어떻게 표현되나?
2. 디스크 위의 ext4 는 실제로 어떤 레이아웃으로 쓰여 있나?
3. `open("/home/woonyong/a.txt")` 는 커널 안에서 몇 단계를 거치나?
4. `read()` 한 번이 VFS -> FS -> page cache -> block layer -> 디스크로 어떻게 흐르나?
5. 소켓·파이프·procfs 가 왜 "파일" 처럼 보이나?

## 답변

### §1. 전체 계층도

```
┌─────────────────────────────────────────────────┐
│ 유저 프로세스                                   │
│   fd = open("/home/a.txt"); read(fd, buf, n);   │
└───────────────┬─────────────────────────────────┘
                │ syscall 트랩 (CPL 3->0)
                v
┌─────────────────────────────────────────────────┐
│ 시스템 콜 계층 (fs/open.c, fs/read_write.c)     │
│   do_sys_openat2 / ksys_read                    │
└───────────────┬─────────────────────────────────┘
                v
┌─────────────────────────────────────────────────┐
│ VFS (fs/namei.c, fs/file_table.c, fs/dcache.c)  │
│   path_lookupat -> do_filp_open -> fd_install     │
│   vfs_read -> file->f_op->read_iter              │
└───────────────┬─────────────────────────────────┘
                v
┌──────┬──────┬──────┬───────────┬────────────────┐
│ ext4 │ xfs  │ btrfs│ tmpfs ... │ 가상 FS        │
│      │      │      │           │ proc/sys/sockfs│
└──────┴──────┴──────┴───────────┴────────────────┘
       │ (디스크 백업 있는 FS 만)
       v
┌─────────────────────────────────────────────────┐
│ 페이지 캐시 (mm/filemap.c)                      │
│   struct address_space, folio                  │
└───────────────┬─────────────────────────────────┘
                v
┌─────────────────────────────────────────────────┐
│ 블록 레이어 (block/)                            │
│   struct bio, struct request, blk-mq            │
└───────────────┬─────────────────────────────────┘
                v
┌─────────────────────────────────────────────────┐
│ 블록 디바이스 드라이버 (drivers/block, nvme, ...)│
└───────────────┬─────────────────────────────────┘
                v
            [ 디스크 / SSD / NVMe ]
```

### §2. VFS 네 개의 핵심 객체

#### super_block — 마운트된 FS 전체

```c
// include/linux/fs.h
struct super_block {
    struct list_head        s_list;           // 전체 SB 리스트
    dev_t                   s_dev;            // 블록 디바이스 번호
    unsigned long           s_blocksize;      // 블록 크기 (보통 4096)
    struct file_system_type *s_type;          // ext4, xfs ...
    const struct super_operations *s_op;      // 메서드 테이블
    struct dentry           *s_root;          // 마운트 루트 dentry
    struct list_head        s_inodes;         // 이 FS 의 inode 목록
    void                    *s_fs_info;       // FS 별 private (ext4_sb_info)
    // ...
};
```

- 메모리에 "마운트 = super_block 1개" 매핑.
- 디스크 위 superblock 블록을 읽어 이 객체를 채움.

#### inode — 파일 본체

```c
struct inode {
    umode_t                 i_mode;            // 타입 + 권한
    kuid_t                  i_uid;
    kgid_t                  i_gid;

    const struct inode_operations *i_op;       // 이름/메타 조작
    struct super_block      *i_sb;

    unsigned long           i_ino;             // inode 번호
    loff_t                  i_size;            // 파일 크기
    struct timespec64       i_atime, i_mtime, i_ctime;

    const struct file_operations *i_fop;       // 기본 f_op

    struct address_space    *i_mapping;        // 페이지 캐시 매핑
    void                    *i_private;        // FS별 (ext4_inode_info)
    // ...
};
```

- 한 파일당 1 개. 하드링크 N 개여도 inode 1 개.
- `i_op` = 이름공간 조작 (create/lookup/unlink/rename).
- `i_fop` = 데이터 I/O (read/write/mmap).

#### dentry — 이름 <-> inode 매핑

```c
struct dentry {
    struct dentry           *d_parent;
    struct qstr             d_name;            // "a.txt"
    struct inode            *d_inode;          // 가리키는 inode
    struct super_block      *d_sb;
    struct list_head        d_child, d_subdirs;
    const struct dentry_operations *d_op;
    // ...
};
```

- 경로 컴포넌트 하나 = dentry 하나. `/home/woonyong/a.txt` -> 4 개.
- **dcache** : 최근 조회한 dentry 를 RCU 해시 테이블로 캐시. 경로 탐색 속도의 핵심.
- 하드링크 = 같은 inode 를 가리키는 dentry 가 여러 개.

#### file — 열린 상태

```c
struct file {
    struct path             f_path;            // dentry + mount
    struct inode            *f_inode;
    const struct file_operations *f_op;

    atomic_long_t           f_count;
    unsigned int            f_flags;           // O_RDONLY, O_APPEND ...
    fmode_t                 f_mode;
    loff_t                  f_pos;             // 읽기/쓰기 오프셋

    struct address_space    *f_mapping;
    void                    *private_data;     // 소켓이면 struct socket*
    // ...
};
```

- 한 번의 `open()` 당 file 1 개. 같은 파일을 두 번 열면 file 2 개 -> offset 독립.
- `fork()` 후 parent/child 는 같은 file 공유 (offset 공유).
- `dup()` 은 같은 file 을 가리키는 fd 추가.

#### 네 객체의 관계

```
task_struct
   └ files_struct
       └ fdtable.fd[]
            [3] ──┐
            [4] ──┼──> struct file ────> struct dentry ────> struct inode
            [5] ──┘                                               │
                                                                  v
                                                           struct super_block
                                                                  │
                                                                  v
                                                            블록 디바이스
```

### §3. ext4 디스크 레이아웃

```
디스크 블록 (4KB 단위)

┌─────────────────────────────────────────────────────────────────┐
│ Block Group 0                                                   │
│   [Superblock] [GDT] [Reserved GDT blocks]                      │
│   [Data Block Bitmap] [Inode Bitmap]                           │
│   [Inode Table   ─── 여러 블록 ───]                             │
│   [Data Blocks   ─── 그룹 나머지 전부 ───]                       │
├─────────────────────────────────────────────────────────────────┤
│ Block Group 1 (같은 구조 반복)                                  │
├─────────────────────────────────────────────────────────────────┤
│ ...                                                             │
└─────────────────────────────────────────────────────────────────┘
```

- **Superblock** : FS 전체 정보 (블록 수, inode 수, 마운트 정보). 그룹 0 외 여러 백업.
- **GDT** : 각 블록 그룹의 메타 (bitmap 위치, free 수).
- **Block Bitmap** : 비트 1 개 = 데이터 블록 1 개의 free/used.
- **Inode Bitmap** : 비트 1 개 = inode 1 개의 free/used.
- **Inode Table** : 실제 inode 구조체 배열 (256B × N개).
- **Data Blocks** : 실제 파일 내용.

#### 디스크 위 ext4 inode 구조

```c
// fs/ext4/ext4.h (요지)
struct ext4_inode {
    __le16  i_mode;
    __le16  i_uid;
    __le32  i_size_lo;
    __le32  i_atime, i_ctime, i_mtime, i_dtime;
    __le16  i_gid;
    __le16  i_links_count;
    __le32  i_blocks_lo;
    __le32  i_flags;
    __le32  i_block[EXT4_N_BLOCKS];   // 15개 — 직접 12 + 간접/이중/삼중
                                      // ext4 는 이 영역을 extent tree 로 재해석
    __le32  i_generation;
    // ...
};
```

#### Extent Tree (ext4 특징)

큰 파일의 "연속 블록 구간" 을 한 엔트리로 표현. ext2/3 의 block pointer 배열보다 훨씬 효율적. `i_block` 영역에 헤더 + 인덱스/리프 노드가 들어가고, 더 크면 B+ 트리로 확장.

```
ext4_extent {
    __le32 ee_block;      // 논리 블록 번호
    __le16 ee_len;        // 연속 길이
    __le16 ee_start_hi;
    __le32 ee_start_lo;   // 디스크 물리 블록 시작
};
```

### §4. 경로 해석 — path_lookupat 내부

`/home/woonyong/a.txt` 한 번을 따라가 보자.

```c
// fs/namei.c (요지)
static int path_lookupat(struct nameidata *nd, unsigned flags,
                         struct path *path)
{
    int err;

    err = path_init(nd, flags);            // 시작점 root / cwd
    if (err) return err;

    while (!(err = link_path_walk(nd->name, nd))) {
        err = lookup_last(nd);
        if (err <= 0) break;
    }
    // ...
}

// 각 컴포넌트를 따라 내려감
static int link_path_walk(const char *name, struct nameidata *nd)
{
    for (;;) {
        hash_len = hash_name(name);

        // 1) dentry cache 빠른 조회
        struct dentry *dentry = __d_lookup_rcu(parent, &qstr);

        if (unlikely(!dentry)) {
            // 2) miss -> 구체 FS 의 lookup 호출
            dentry = __lookup_slow(name, parent, flags);
            // -> i_op->lookup = ext4_lookup
            //     -> 디렉토리 블록 읽기 -> htree 검색
            //     -> inode 로드 -> dentry 생성
            //     -> dcache 에 삽입
        }

        // 3) 마운트 포인트 / symlink 처리
        //    ...
        nd->path.dentry = dentry;
        name = next_component;
    }
}
```

#### 실제 흐름

```
/         -> dcache hit  (root dentry)
home      -> dcache hit  (자주 접근돼 항상 캐시됨)
woonyong  -> dcache hit
a.txt     -> 처음이면 miss
            └ ext4_lookup
                 └ ext4_find_entry
                     └ 디렉토리 inode 의 블록 읽기
                     └ HTree 인덱스 해시 검색
                     └ "a.txt -> inode #132045" 찾음
                 └ ext4_iget(132045)
                     └ inode bitmap 으로 그룹 찾기
                     └ inode table 에서 256B 읽기
                     └ struct inode 만들기
                 └ d_splice_alias 로 dentry 와 연결
                 └ dcache 에 삽입
```

### §5. open() 완전 경로

```c
// fs/open.c
SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename,
                int, flags, umode_t, mode)
{
    return do_sys_openat2(dfd, filename, &how);
}

static long do_sys_openat2(int dfd, const char __user *filename,
                           struct open_how *how)
{
    struct filename *tmp = getname(filename);          // 유저 -> 커널
    int fd = get_unused_fd_flags(how->flags);          // fdtable 빈 슬롯
    struct file *f = do_filp_open(dfd, tmp, &op);
    //   └ path_openat -> path_lookupat -> dentry
    //   └ vfs_open -> dentry_open
    //       └ alloc_empty_file
    //       └ f->f_op = file->f_inode->i_fop
    //       └ f_op->open(inode, f)  -> ext4_file_open
    fd_install(fd, f);                                 // fdtable[fd] = f
    return fd;
}
```

단계를 정리하면:

```
[1] getname                : 유저가 준 문자열 -> 커널 메모리
[2] get_unused_fd_flags    : 가장 작은 빈 fd 인덱스 확보
[3] path_lookupat          : 경로 -> dentry
[4] alloc_empty_file       : struct file (filp slab) 에서 할당
[5] vfs_open               : FS 의 f_op->open 호출 (ext4_file_open)
[6] file->f_op             : 해당 FS 의 f_op 로 설정
[7] fd_install             : fdtable[fd] = file
[8] return fd              : 유저에게 반환
```

### §6. read() 완전 경로

```c
// fs/read_write.c
SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
    return ksys_read(fd, buf, count);
}

ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
    struct fd f = fdget_pos(fd);                       // fdtable[fd]
    if (!f.file) return -EBADF;
    loff_t pos = file_pos_read(f.file);
    ssize_t ret = vfs_read(f.file, buf, count, &pos);
    file_pos_write(f.file, pos);
    fdput_pos(f);
    return ret;
}

ssize_t vfs_read(struct file *file, char __user *buf,
                 size_t count, loff_t *pos)
{
    if (file->f_op->read)
        return file->f_op->read(file, buf, count, pos);
    if (file->f_op->read_iter)
        return new_sync_read(file, buf, count, pos);
    return -EINVAL;
}
```

ext4 의 경우 `read_iter = ext4_file_read_iter`:

```c
// fs/ext4/file.c
static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    // 대부분 일반 경로
    return generic_file_read_iter(iocb, to);
}
```

#### 페이지 캐시 조회 — filemap_read

```c
// mm/filemap.c (요지)
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter, ssize_t ret)
{
    struct file *filp = iocb->ki_filp;
    struct address_space *mapping = filp->f_mapping;
    loff_t pos = iocb->ki_pos;
    pgoff_t index = pos >> PAGE_SHIFT;

    for (;;) {
        // 1) 페이지 캐시에서 해당 offset 의 folio 찾기
        struct folio *folio = filemap_get_folio(mapping, index);

        if (!folio) {
            // 2) miss -> 디스크에서 가져오기
            error = filemap_create_folio(filp, mapping, index, fbatch);
            // -> add_to_page_cache_lru
            // -> mapping->a_ops->read_folio / readahead
            //   -> FS의 read_folio -> submit_bio(READ)
        }

        // 3) folio 에서 유저 버퍼로 복사
        copied = copy_folio_to_iter(folio, offset, bytes, iter);
        // ...
    }
}
```

#### address_space_operations — FS 가 구현

```c
// fs/ext4/inode.c
const struct address_space_operations ext4_aops = {
    .read_folio        = ext4_read_folio,
    .readahead         = ext4_readahead,
    .writepages        = ext4_writepages,
    .dirty_folio       = ext4_dirty_folio,
    .release_folio     = ext4_release_folio,
    // ...
};
```

`ext4_readahead` 는 여러 페이지를 배치로 읽어 `submit_bio` 로 블록 레이어에 넘김.

### §7. 블록 레이어 — BIO 와 request

```c
// include/linux/blk_types.h
struct bio {
    struct bio          *bi_next;
    struct block_device *bi_bdev;
    blk_opf_t           bi_opf;             // READ/WRITE + flags
    struct bvec_iter    bi_iter;            // 어느 섹터부터 얼마나
    struct bio_vec      *bi_io_vec;         // 페이지 목록
    bio_end_io_t        *bi_end_io;         // 완료 콜백
    void                *bi_private;
    // ...
};
```

#### BIO 제출

```c
void submit_bio(struct bio *bio)
{
    submit_bio_noacct(bio);
    // blk-mq: per-CPU software queue -> hardware queue
    //   -> 드라이버 queue_rq (NVMe, SATA ...)
}
```

#### blk-mq (multi-queue block layer)

```
CPU 0 ─ software queue 0 ─┐
CPU 1 ─ software queue 1 ─┼─ tag 할당 ─ hardware queue ─ 디바이스
CPU 2 ─ software queue 2 ─┤
CPU 3 ─ software queue 3 ─┘
```

- NVMe 는 내부 큐가 많아 OS 큐도 여러 개.
- I/O 스케줄러 : mq-deadline, kyber, bfq, none — request 병합·정렬·공평성.

#### 디스크 완료

```
디스크 완료 -> IRQ -> 드라이버 completion 핸들러
    └ bio->bi_end_io 호출
        └ folio 에 Uptodate 플래그 + unlock
            └ filemap_read 에서 대기 중이던 프로세스 깨움
                └ copy_folio_to_iter -> 유저 버퍼
                    └ 유저 프로세스 return
```

### §8. write() 경로와 writeback

```c
// 일반 write (O_DIRECT 아님)
ssize_t generic_perform_write(...)
{
    for (;;) {
        // 1) 페이지 캐시에서 페이지 확보 (없으면 생성)
        folio = filemap_get_folio(mapping, index);

        // 2) 유저 버퍼를 페이지에 복사
        copied = copy_folio_from_iter_atomic(...);

        // 3) dirty 마킹 (아직 디스크엔 쓰지 않음)
        folio_mark_dirty(folio);
    }
}
```

실제 디스크 쓰기는 **writeback 커널 스레드** 가 수행:

```c
// mm/page-writeback.c, fs/fs-writeback.c
// 커널 스레드: [kworker/u.../writeback]

트리거 :
  - dirty_writeback_centisecs (기본 5초)
  - dirty_ratio (기본 20%) 넘음
  - fsync() / sync() 호출
-> balance_dirty_pages
-> writeback_inodes_sb
-> __writeback_single_inode
-> mapping->a_ops->writepages   // ext4_writepages
-> submit_bio(WRITE)
```

`fsync(fd)` : 해당 inode 의 dirty 페이지 모두 디스크 flush 후 반환. 데이터베이스의 durability 보장의 근거.

### §9. 마운트 메커니즘

```c
// fs/namespace.c (요지)
SYSCALL_DEFINE5(mount, char __user *, dev_name, char __user *, dir_name,
                char __user *, type, unsigned long, flags,
                void __user *, data)
{
    return do_mount(kernel_dev, dir_name, kernel_type, flags, options);
}

long do_mount(const char *dev_name, const char __user *dir_name, ...)
{
    // 1) file_system_type 찾기 (등록된 FS)
    struct file_system_type *type = get_fs_type(fstype);

    // 2) superblock 구성
    struct super_block *sb = type->mount(type, flags, dev_name, data);
    //   ext4_mount
    //     -> ext4_fill_super
    //         -> 디스크 superblock 읽기
    //         -> 루트 inode (#2) 로드
    //         -> s_root = d_make_root(root_inode)

    // 3) vfsmount 만들고 네임스페이스 트리에 붙이기
    vfs_create_mount(fc) -> graft_tree -> attach_recursive_mnt
}
```

마운트 포인트 dentry 를 따라가면 해당 지점에서 **새 FS 의 루트 dentry 로 자동 전환**.

### §10. 가상 / 메모리 파일시스템

| FS | 위치 | 역할 |
| --- | --- | --- |
| **procfs** | `/proc` | 프로세스/커널 정보 (`/proc/<pid>/status`, `/proc/meminfo`) |
| **sysfs** | `/sys` | 커널 객체 트리 (디바이스, 모듈) |
| **tmpfs** | `/tmp`, `/dev/shm` | RAM 기반 임시 파일 |
| **devtmpfs** | `/dev` | 디바이스 노드 자동 생성 |
| **pipefs** | 숨김 | `pipe()` 구현 기반 |
| **sockfs** | 숨김 | `socket()` 구현 기반 |
| **debugfs** | `/sys/kernel/debug` | 커널 디버그 노출 |
| **cgroupfs** | `/sys/fs/cgroup` | cgroup 트리 |

디스크에 데이터가 없고 메모리만으로 동작. VFS 위에 "가짜 FS" 로 올려두는 이유는 **읽기/쓰기 인터페이스를 파일 하나로 통일** 하기 위함.

### §11. sockfs — 소켓이 VFS 에 끼워지는 방법

```c
// net/socket.c
static struct file_system_type sock_fs_type = {
    .name    = "sockfs",
    .init_fs_context = sockfs_init_fs_context,
    .kill_sb = kill_anon_super,
};

static int __init sock_init(void)
{
    init_inodecache();
    err = register_filesystem(&sock_fs_type);
    sock_mnt = kern_mount(&sock_fs_type);
    // ...
}
```

#### socket(AF_INET, SOCK_STREAM, 0) 흐름

```c
SYSCALL_DEFINE3(socket, int, family, int, type, int, protocol)
{
    int retval = sock_create(family, type, protocol, &sock);
    //   -> sock_alloc
    //       └ new_inode_pseudo(sock_mnt->mnt_sb)   // anon inode
    //       └ inode->i_op = &sockfs_inode_ops
    //       └ socket = SOCKET_I(inode)             // inode 안의 socket 얻기
    //   -> pf->create (AF_INET -> inet_create)
    //       └ sk_alloc -> struct sock 생성
    //       └ sock->ops = &inet_stream_ops

    retval = sock_map_fd(sock, flags);
    //   -> get_unused_fd_flags -> 빈 fd
    //   -> sock_alloc_file
    //       └ alloc_file_pseudo(inode, mnt, dname, flags, &socket_file_ops)
    //       └ file->private_data = sock
    //   -> fd_install(fd, file)
    return retval;
}
```

#### socket_file_ops

```c
// net/socket.c
static const struct file_operations socket_file_ops = {
    .owner          = THIS_MODULE,
    .llseek         = no_llseek,
    .read_iter      = sock_read_iter,
    .write_iter     = sock_write_iter,
    .poll           = sock_poll,
    .unlocked_ioctl = sock_ioctl,
    .mmap           = sock_mmap,
    .release        = sock_close,
    // ...
};
```

그래서 `read(sockfd, buf, n)` 은:

```
read(sockfd, buf, n)
  └ ksys_read -> vfs_read
      └ file->f_op->read_iter = sock_read_iter
          └ sock_recvmsg
              └ sock->ops->recvmsg = tcp_recvmsg   (TCP)
                  └ sk_receive_queue 에서 skb 꺼내 copy_to_user
```

### §12. f_op 디스패치 — 같은 read() 가 왜 다르게 동작하나

```
fd=3 (ext4 파일)
  file->f_op = ext4_file_operations
      read_iter = ext4_file_read_iter
          -> generic_file_read_iter -> filemap_read
              -> page cache -> (miss) -> block layer -> 디스크

fd=4 (TCP 소켓)
  file->f_op = socket_file_ops
      read_iter = sock_read_iter
          -> sock_recvmsg -> tcp_recvmsg
              -> sk_receive_queue -> skb 의 payload 복사

fd=5 (pipe)
  file->f_op = pipefifo_fops
      read_iter = pipe_read
          -> 내부 링버퍼에서 복사 -> writer wake

fd=6 (/dev/null)
  file->f_op = null_fops
      read_iter = read_null
          -> 그냥 0 반환

fd=7 (/proc/meminfo)
  file->f_op = proc_single_file_operations
      read_iter = seq_read_iter
          -> 동적으로 문자열 생성해서 복사
```

시스템콜 `read()` 자체는 **정확히 같은 함수**. 동작 차이의 실체는 `file->f_op->read_iter` 가 가리키는 함수가 다를 뿐. 이게 **"모든 것은 파일" 의 구현 비밀**.

### §13. 한 파일 열고 읽기 — 전체 트레이스

```
user: fd = open("/home/a.txt", O_RDONLY)

 1. SYSCALL openat -> do_sys_openat2
 2. getname                 : 유저 문자열 -> 커널
 3. get_unused_fd_flags     : fd = 3
 4. path_lookupat
     ├ "/"         dcache hit
     ├ "home"      dcache hit
     └ "a.txt"     dcache miss
         └ ext4_lookup -> ext4_find_entry -> ext4_iget
         └ struct inode, struct dentry 생성
 5. alloc_empty_file        : struct file (filp slab)
 6. ext4_file_open          : f_op->open
 7. fd_install(3, file)
 -> fd=3 반환

user: n = read(3, buf, 4096)

 1. SYSCALL read -> ksys_read
 2. fdget(3)                : struct file*
 3. vfs_read -> file->f_op->read_iter = ext4_file_read_iter
 4. generic_file_read_iter -> filemap_read
 5. filemap_get_folio(mapping, index=0)
     └ miss -> filemap_create_folio
         └ add_to_page_cache_lru
         └ ext4_read_folio
             └ ext4_map_blocks (논리 블록 -> 물리 블록)
                 └ extent tree 조회
             └ submit_bio(READ)
 6. 블록 레이어
     └ blk_mq_submit_bio -> request queue
     └ 드라이버 queue_rq (NVMe, SATA)
     └ 디스크 I/O 개시
 7. 디스크 완료 IRQ
     └ bio_end_io 콜백
     └ folio Uptodate + unlock
 8. filemap_read 재개
     └ copy_folio_to_iter(folio, offset, 4096, iter)
 9. 유저 공간에 데이터 도착
```

### §14. 슬랩 캐시 — 자주 보이는 인스턴스들

`cat /proc/slabinfo` 로 볼 수 있는 FS 관련 캐시:

```
# name            <active_objs> <num_objs> <objsize> ...
dentry                 245120     245760       192
inode_cache             42384      42384       592
ext4_inode_cache        78392      78400      1024
shmem_inode_cache        1024       1024       760
filp                    10304      10304       256
buffer_head            156823     158080       104
```

- `filp` = struct file 캐시
- `dentry`, `inode_cache` = VFS 객체
- `ext4_inode_cache` = ext4 전용 inode (일반 inode 에 덧붙여지는 부분)
- `buffer_head` = 블록 레이어 버퍼

## 연결 키워드

- [02-keyword-tree.md — 10장 시스템 I/O](../../csapp-10/)
- [q05-socket-principle.md](./q05-socket-principle.md) — 소켓이 파일로 보이는 이유 (개괄)
- [q09-network-cpu-kernel-handle.md](./q09-network-cpu-kernel-handle.md) — 네 개의 렌즈 종합
- [q10-io-bridge.md](./q10-io-bridge.md) — 블록 레이어 아래 하드웨어·PCIe 경로
- [q18-thread-concurrency.md](./q18-thread-concurrency.md) — 페이지 캐시/블록 레이어에서 쓰이는 락과 RCU
