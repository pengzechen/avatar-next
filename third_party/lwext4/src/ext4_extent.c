/*
 * src/ext4_extent.c — ext4 extent tree
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * Avatar OS 干净室重写声明（clean-room rewrite）
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * lwext4 上游有两个文件是 GPLv2（src/ext4_extent.c、src/ext4_xattr.c），
 * 上游 README 明说它们"使整个 lwext4 变成 GPLv2"。本项目要把 lwext4 收进
 * 仓库并保持纯 BSD-3-Clause，因此：
 *
 *   - src/ext4_xattr.c  —— 整块移除（本内核不暴露 xattr API）
 *   - src/ext4_extent.c  —— 即本文件，按**公开的 on-disk 格式规范**重写
 *
 * 本文件依据的只有：
 *   1. 公开的 ext4 on-disk 格式规范
 *      （Linux 内核 Documentation/filesystems/ext4/extents.rst。
 *        磁盘格式是公开事实，不受版权保护。）
 *   2. BSD-3-Clause 的接口声明 include/ext4_extent.h
 *   3. BSD-3-Clause 的调用方契约（src/ext4_fs.c 的三处调用）与依赖
 *      （ext4_balloc / ext4_trans / ext4_crc32 / ext4_inode / ext4_super /
 *        ext4_bcache —— 全部 BSD-3-Clause）
 *
 * 重写时**没有阅读原 ext4_extent.c 的实现体**；函数签名与语义遵守
 * ext4_extent.h，实现独立完成。
 *
 * ── on-disk 布局 ──────────────────────────────────────────────────────
 *
 *   extent 节点 = 头(12B) + 若干条目(每条 12B) + [可选] 尾(4B)
 *
 *   头： magic = 0xF30A, entries, max, depth, generation
 *   depth == 0 → 条目为 struct ext4_extent（叶子）
 *   depth >  0 → 条目为 struct ext4_extent_idx（指向下一层物理块）
 *
 *   索引项按 ei_block 升序，第 i 项管辖 [ei_block[i], ei_block[i+1])；
 *   叶子项按 ee_block 升序，覆盖 [ee_block, ee_block + len)。
 *
 *   ee_len 的 bit15 是"未初始化"标志：ee_len > 32768 表示未初始化，
 *   真实长度 = ee_len - 32768。**ee_len == 32768 是已初始化的 32768**，
 *   不是"未初始化的 0"——取长度必须按这个边界判，否则会算成 0。
 *
 *   根内嵌在 inode 的 i_block[15]（60 字节）里，容量固定
 *   (60 - 12) / 12 = 4；非根节点容量 = (block_size - 12 - [csum?4:0]) / 12。
 *   非根节点在 metadata_csum 打开时末尾 4 字节是 crc32c，覆盖前
 *   (block_size - 4) 字节。
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>
#include <ext4_trans.h>
#include <ext4_fs.h>
#include <ext4_blockdev.h>
#include <ext4_super.h>
#include <ext4_crc32.h>
#include <ext4_balloc.h>
#include <ext4_inode.h>
#include <ext4_extent.h>
#include <ext4_bcache.h>

#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE

/* ── 常量 ─────────────────────────────────────────────────────────────── */

#define EXT4_EXTENT_MAGIC 0xF30Au

/* ee_len 的 bit15：超过它表示"未初始化 extent" */
#define EXT4_EXTENT_LEN_INIT 0x8000u

/* 规范允许的树深上限；实际文件系统到 5 已是天文数字 */
#define EXT4_EXTENT_MAX_DEPTH 5

/* ── on-disk 结构（按规范定义，非从原实现抄来）────────────────────────── */

struct ext4_extent_header {
	uint16_t eh_magic;
	uint16_t eh_entries;
	uint16_t eh_max;
	uint16_t eh_depth;
	uint32_t eh_generation;
};

struct ext4_extent {
	uint32_t ee_block;    /* 覆盖的第一个逻辑块 */
	uint16_t ee_len;      /* 块数（bit15 = 未初始化标志）*/
	uint16_t ee_start_hi; /* 起始物理块高 16 位 */
	uint32_t ee_start_lo; /* 起始物理块低 32 位 */
};

struct ext4_extent_idx {
	uint32_t ei_block;   /* 覆盖的第一个逻辑块 */
	uint32_t ei_leaf_lo; /* 下一层节点的物理块低 32 位 */
	uint16_t ei_leaf_hi; /* 高 16 位 */
	uint16_t ei_unused;
};

struct ext4_extent_tail {
	uint32_t et_checksum;
};

/* ── 小工具 ───────────────────────────────────────────────────────────── */

static inline bool extent_has_csum(struct ext4_sblock *sb)
{
	return ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM);
}

static inline uint32_t extent_root_capacity(void)
{
	return (uint32_t)((EXT4_INODE_BLOCKS * sizeof(uint32_t) -
			   sizeof(struct ext4_extent_header)) /
			  sizeof(struct ext4_extent));
}

static uint32_t extent_node_capacity(struct ext4_sblock *sb)
{
	uint32_t bs = ext4_sb_get_block_size(sb);
	uint32_t avail = bs - (uint32_t)sizeof(struct ext4_extent_header);

	if (extent_has_csum(sb))
		avail -= (uint32_t)sizeof(struct ext4_extent_tail);
	return avail / (uint32_t)sizeof(struct ext4_extent);
}

static inline struct ext4_extent *extent_ents(struct ext4_extent_header *h)
{
	return (struct ext4_extent *)(h + 1);
}

static inline struct ext4_extent_idx *extent_idxs(struct ext4_extent_header *h)
{
	return (struct ext4_extent_idx *)(h + 1);
}

static inline ext4_fsblk_t extent_start(struct ext4_extent *e)
{
	return ((ext4_fsblk_t)e->ee_start_hi << 32) | (ext4_fsblk_t)e->ee_start_lo;
}

static inline void extent_set_start(struct ext4_extent *e, ext4_fsblk_t b)
{
	e->ee_start_hi = (uint16_t)(b >> 32);
	e->ee_start_lo = (uint32_t)(b & 0xFFFFFFFFu);
}

static inline uint32_t extent_len(struct ext4_extent *e)
{
	return (e->ee_len > EXT4_EXTENT_LEN_INIT)
		   ? (uint32_t)(e->ee_len - EXT4_EXTENT_LEN_INIT)
		   : (uint32_t)e->ee_len;
}

static inline void extent_set_len(struct ext4_extent *e, uint32_t len)
{
	e->ee_len = (uint16_t)(len & 0x7FFFu);
}

static inline bool extent_is_uninit(struct ext4_extent *e)
{
	return e->ee_len > EXT4_EXTENT_LEN_INIT;
}

static inline ext4_fsblk_t extent_idx_leaf(struct ext4_extent_idx *i)
{
	return ((ext4_fsblk_t)i->ei_leaf_hi << 32) | (ext4_fsblk_t)i->ei_leaf_lo;
}

static inline void extent_idx_set_leaf(struct ext4_extent_idx *i, ext4_fsblk_t b)
{
	i->ei_leaf_hi = (uint16_t)(b >> 32);
	i->ei_leaf_lo = (uint32_t)(b & 0xFFFFFFFFu);
}

/* 只有非根节点有校验尾；根在 inode 里，由 inode 的校验和覆盖 */
static void extent_node_csum(struct ext4_sblock *sb, uint8_t *node)
{
	if (!extent_has_csum(sb))
		return;

	uint32_t bs = ext4_sb_get_block_size(sb);
	struct ext4_extent_tail *tail;
	tail = (struct ext4_extent_tail *)(node + bs - sizeof(*tail));
	tail->et_checksum = ext4_crc32c(EXT4_CRC32_INIT, node,
					bs - (uint32_t)sizeof(*tail));
}

/*
 * 二分查找：最后一次满足 key[i] <= iblock 的下标；全都大于则 -1。
 *
 * ⚠️ 必须按**结构体数组**遍历，不能把它降级成 uint32_t[] 去下标 ——
 * extent / idx 条目都是 12 字节的步长，当成 4 字节数组读，第 1 项拿到的是
 * (ee_len|ee_start_hi) 拼出来的垃圾，二分从第一步就走错分支。
 * （这是本文件第一版的实际 bug：读 busybox 的第一条 extent 就内核态 #PF。）
 */
static int extent_bsearch(const void *arr, size_t stride, uint32_t count,
			  ext4_lblk_t iblock, bool is_leaf)
{
	int lo = 0;
	int hi = (int)count - 1;
	int hit = -1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		const uint8_t *p = (const uint8_t *)arr + (size_t)mid * stride;
		uint32_t key = is_leaf
				       ? ((const struct ext4_extent *)p)->ee_block
				       : ((const struct ext4_extent_idx *)p)->ei_block;
		if (key <= iblock) {
			hit = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return hit;
}

/* 在叶子里定位 iblock；*inside 表示 ents[i] 是否真的覆盖它 */
static int extent_leaf_search(struct ext4_extent_header *leaf, ext4_lblk_t iblock,
			      bool *inside)
{
	struct ext4_extent *ents = extent_ents(leaf);
	int i = extent_bsearch(ents, sizeof(*ents), leaf->eh_entries, iblock,
			       true);

	*inside = false;
	if (i < 0)
		return -1;

	if ((uint64_t)iblock < (uint64_t)ents[i].ee_block + extent_len(&ents[i]))
		*inside = true;
	return i;
}

static int extent_idx_search(struct ext4_extent_header *node, ext4_lblk_t iblock)
{
	struct ext4_extent_idx *idxs = extent_idxs(node);
	return extent_bsearch(idxs, sizeof(*idxs), node->eh_entries, iblock,
			      false);
}

/* ── 遍历路径 ─────────────────────────────────────────────────────────── *
 *
 * create 路径要"从叶子往上改"（叶子满了分裂、把索引项插进父节点……），
 * 所以先把整条路径记下来。depth ≤ 5 → 最长 6 层。
 */
struct extent_path {
	struct ext4_block blk;          /* 该层所在的块（根为 in_inode）*/
	struct ext4_extent_header *hdr; /* 该层的头 */
	uint8_t *raw;                   /* 该层缓冲起点（算 csum 用），根为 NULL */
	uint32_t index;                 /* 下一层在本层的下标 */
	bool in_inode;                  /* true = 根（数据在 inode->blocks）*/
	bool valid;
};

static void extent_path_put(struct ext4_fs *fs, struct extent_path *path, int n)
{
	for (int i = 0; i < n; i++) {
		if (path[i].valid && !path[i].in_inode)
			ext4_block_set(fs->bdev, &path[i].blk);
	}
}

/* 从根走到叶子，填 path[]；返回叶子层号（>=0）或负错误码 */
static int extent_walk(struct ext4_fs *fs, struct ext4_inode *inode,
		       ext4_lblk_t iblock, struct extent_path *path)
{
	struct ext4_extent_header *hdr = ext4_inode_get_extent_header(inode);
	int level = 0;

	if (hdr->eh_magic != EXT4_EXTENT_MAGIC)
		return -EIO;
	if (hdr->eh_depth > EXT4_EXTENT_MAX_DEPTH)
		return -EIO;

	path[0].in_inode = true;
	path[0].raw = NULL;
	path[0].hdr = hdr;
	path[0].valid = true;
	path[0].index = 0;

	while (path[level].hdr->eh_depth > 0) {
		int i = extent_idx_search(path[level].hdr, iblock);
		struct ext4_extent_idx *idxs;
		struct extent_path *next;
		ext4_fsblk_t child;
		int rc;

		if (i < 0 || level + 1 > EXT4_EXTENT_MAX_DEPTH)
			return -EIO; /* 索引不覆盖 iblock：树已损坏 */

		idxs = extent_idxs(path[level].hdr);
		child = extent_idx_leaf(&idxs[i]);

		next = &path[level + 1];
		rc = ext4_trans_block_get(fs->bdev, &next->blk, child);
		if (rc != EOK)
			return -rc;

		next->hdr = (struct ext4_extent_header *)next->blk.data;
		next->raw = next->blk.data;
		next->in_inode = false;
		next->valid = true;
		next->index = 0;
		path[level].index = (uint32_t)i;
		level++;
	}
	return level;
}

/* 分配并初始化一个空的树节点 */
static int extent_alloc_node(struct ext4_inode_ref *iref, struct ext4_block *blk,
			     ext4_fsblk_t *out_phys, uint16_t depth)
{
	struct ext4_fs *fs = iref->fs;
	struct ext4_extent_header *h;
	ext4_fsblk_t phys;
	int rc;

	rc = ext4_balloc_alloc_block(iref, ext4_fs_inode_to_goal_block(iref),
				     &phys);
	if (rc != EOK)
		return rc;
	iref->dirty = true;

	/* 新块不必从盘上读，但必须显式标 UPTODATE，否则写下去的是垃圾 */
	rc = ext4_trans_block_get_noread(fs->bdev, blk, phys);
	if (rc != EOK)
		return rc;

	memset(blk->data, 0, ext4_sb_get_block_size(&fs->sb));
	h = (struct ext4_extent_header *)blk->data;
	h->eh_magic = EXT4_EXTENT_MAGIC;
	h->eh_max = (uint16_t)extent_node_capacity(&fs->sb);
	h->eh_depth = depth;

	*out_phys = phys;
	return EOK;
}

/* 把一条索引项插进节点（调用方须保证有空位） */
static void extent_index_insert(struct ext4_extent_header *node, int at,
				struct ext4_extent_idx *entry)
{
	struct ext4_extent_idx *idxs = extent_idxs(node);
	uint32_t n = node->eh_entries;
	int pos = at + 1;

	memmove(&idxs[pos + 1], &idxs[pos],
		(size_t)(n - (uint32_t)pos) * sizeof(struct ext4_extent_idx));
	idxs[pos] = *entry;
	node->eh_entries = (uint16_t)(n + 1);
}

/*
 * 把一个节点（叶子或索引）对半分裂。后半搬进新节点，前半留下；
 * *out 返回描述新节点的索引项。
 */
static int extent_split_node(struct ext4_inode_ref *iref,
			     struct ext4_extent_header *node,
			     struct ext4_extent_idx *out)
{
	struct ext4_sblock *sb = &iref->fs->sb;
	uint32_t total = node->eh_entries;
	uint32_t half = total / 2;
	uint32_t moved = total - half;
	struct ext4_block nb;
	struct ext4_extent_header *nh;
	ext4_fsblk_t nphys;
	int rc;

	rc = extent_alloc_node(iref, &nb, &nphys, node->eh_depth);
	if (rc != EOK)
		return rc;

	nh = (struct ext4_extent_header *)nb.data;
	nh->eh_entries = (uint16_t)moved;
	memcpy((uint8_t *)nh + sizeof(struct ext4_extent_header),
	       (uint8_t *)node + sizeof(struct ext4_extent_header) +
		       (size_t)half * sizeof(struct ext4_extent),
	       (size_t)moved * sizeof(struct ext4_extent));

	node->eh_entries = (uint16_t)half;

	if (node->eh_depth == 0) {
		struct ext4_extent *first = extent_ents(nh);
		out->ei_block = first->ee_block;
	} else {
		struct ext4_extent_idx *first = extent_idxs(nh);
		out->ei_block = first->ei_block;
	}
	out->ei_unused = 0;
	extent_idx_set_leaf(out, nphys);

	extent_node_csum(sb, nb.data);
	ext4_trans_set_block_dirty(nb.buf);
	ext4_block_set(iref->fs->bdev, &nb);
	return EOK;
}

/*
 * 从第 level 层往上插一条索引项（entry 要落在 path[level-1] 的
 * path[level-1].index 之后）。沿途节点满了就对半分裂再往上；根满了长高一层。
 */
static int extent_propagate(struct ext4_inode_ref *iref,
			    struct extent_path *path, int level,
			    struct ext4_extent_idx *entry)
{
	struct ext4_sblock *sb = &iref->fs->sb;
	struct ext4_extent_idx ins = *entry;
	struct ext4_extent_header *node = path[level - 1].hdr;

	for (;;) {
		if (node->eh_entries < node->eh_max) {
			extent_index_insert(node, (int)path[level - 1].index,
					    &ins);
			if (path[level - 1].in_inode)
				iref->dirty = true;
			else
				extent_node_csum(sb, path[level - 1].raw);
			return EOK;
		}

		/* 满了 → 分裂，产生要插到再上一层的项 */
		struct ext4_extent_idx up;
		int rc = extent_split_node(iref, node, &up);

		if (rc != EOK)
			return rc;

		/* ins 该落进前半还是后半？拿 ei_block 与分界比 */
		if (ins.ei_block < up.ei_block) {
			struct ext4_extent_idx *idxs = extent_idxs(node);
			int at = extent_bsearch(idxs, sizeof(*idxs),
						node->eh_entries, ins.ei_block, false);
			extent_index_insert(node, at, &ins);
			if (path[level - 1].in_inode)
				iref->dirty = true;
			else
				extent_node_csum(sb, path[level - 1].raw);
		} else {
			/* 落进新节点。它刚分裂出来，用它自己的容量上限判断
			 * 是否还满——满了就把问题转成"往新节点插"继续分裂。 */
			struct ext4_block nb;
			struct ext4_extent_header *nh;
			struct ext4_extent_idx *idxs;
			int at;

			rc = ext4_trans_block_get(iref->fs->bdev, &nb,
						  extent_idx_leaf(&up));
			if (rc != EOK)
				return -rc;

			nh = (struct ext4_extent_header *)nb.data;
			idxs = extent_idxs(nh);
			at = extent_bsearch(idxs, sizeof(*idxs), nh->eh_entries,
					    ins.ei_block, false);

			if (nh->eh_entries >= nh->eh_max) {
				/* 极少见：新节点也满（分裂点两侧都挤满了）。
				 * 直接放弃这次插入会丢块，所以退化为"先放进去
				 * 再说"不可行 —— 只能返回 EIO 让上层回滚。 */
				ext4_block_set(iref->fs->bdev, &nb);
				return EIO;
			}

			extent_index_insert(nh, at, &ins);
			extent_node_csum(sb, nh);
			ext4_trans_set_block_dirty(nb.buf);
			ext4_block_set(iref->fs->bdev, &nb);
		}

		ins = up;

		if (level - 1 == 0)
			break; /* 到根了，用下面的长高逻辑 */
		level--;
		node = path[level - 1].hdr;
	}

	/* 根也满了：整根下沉到新节点，根变成 depth+1、只有一条索引项 */
	{
		struct ext4_extent_header *root = path[0].hdr;
		uint8_t saved[EXT4_INODE_BLOCKS * sizeof(uint32_t)];
		struct ext4_block nb;
		struct ext4_extent_header *nh;
		ext4_fsblk_t nphys;
		int rc;

		memcpy(saved, root, sizeof(saved));

		rc = extent_alloc_node(iref, &nb, &nphys,
				       (uint16_t)(root->eh_depth));
		if (rc != EOK)
			return rc;

		nh = (struct ext4_extent_header *)nb.data;
		nh->eh_entries = root->eh_entries;
		memcpy((uint8_t *)nh + sizeof(struct ext4_extent_header),
		       saved + sizeof(struct ext4_extent_header),
		       sizeof(saved) - sizeof(struct ext4_extent_header));

		memset(root, 0, EXT4_INODE_BLOCKS * sizeof(uint32_t));
		root->eh_magic = EXT4_EXTENT_MAGIC;
		root->eh_max = (uint16_t)extent_root_capacity();
		root->eh_depth = (uint16_t)(nh->eh_depth + 1);
		root->eh_entries = 1;

		struct ext4_extent_idx *ri = extent_idxs(root);
		ri[0].ei_block = 0;
		ri[0].ei_unused = 0;
		extent_idx_set_leaf(&ri[0], nphys);

		extent_node_csum(sb, nb.data);
		ext4_trans_set_block_dirty(nb.buf);
		ext4_block_set(iref->fs->bdev, &nb);

		/*
		 * 根现在只有 1 条（指向 nh，装的是旧根的下半）。ins 是刚才
		 * 分裂出的上半，其 ei_block 大于 nh 里所有块的起始 —— 所以它
		 * 必然排在 nh 之后，插到根的末尾即可。此时根必有空位
		 * （容量 4，刚用了 1 条）。
		 */
		extent_index_insert(root, (int)root->eh_entries - 1, &ins);

		iref->dirty = true;
	}

	return EOK;
}

/* ── 对外：tree_init ──────────────────────────────────────────────────── */

void ext4_extent_tree_init(struct ext4_inode_ref *inode_ref)
{
	struct ext4_extent_header *hdr =
		ext4_inode_get_extent_header(inode_ref->inode);

	memset(hdr, 0, EXT4_INODE_BLOCKS * sizeof(uint32_t));
	hdr->eh_magic = EXT4_EXTENT_MAGIC;
	hdr->eh_max = (uint16_t)extent_root_capacity();
	hdr->eh_depth = 0;
	hdr->eh_entries = 0;

	inode_ref->dirty = true;
}

/* ── 对外：get_blocks ─────────────────────────────────────────────────── */

int ext4_extent_get_blocks(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
			   uint32_t max_blocks, ext4_fsblk_t *result, bool create,
			   uint32_t *blocks_count)
{
	struct ext4_fs *fs = inode_ref->fs;
	struct extent_path path[EXT4_EXTENT_MAX_DEPTH + 1];
	int level;
	bool inside;
	int at;

	if (result == NULL)
		return EINVAL;

	*result = 0;
	if (blocks_count)
		*blocks_count = 0;

	level = extent_walk(fs, inode_ref->inode, iblock, path);
	if (level < 0)
		return -level;

	at = extent_leaf_search(path[level].hdr, iblock, &inside);

	if (inside) {
		struct ext4_extent *e = &extent_ents(path[level].hdr)[at];
		uint32_t off = iblock - e->ee_block;
		uint32_t avail = extent_len(e) - off;

		*result = extent_start(e) + off;
		if (blocks_count)
			*blocks_count = (max_blocks && avail > max_blocks)
						? max_blocks
						: avail;
		extent_path_put(fs, path, level + 1);
		return EOK;
	}

	if (!create) {
		/* 洞：*result 保持 0，调用方按 support_unwritten 处理 */
		extent_path_put(fs, path, level + 1);
		return EOK;
	}

	/* 需要新块。先分配，再确保叶子有空位，最后插入。 */
	ext4_fsblk_t pblock;
	int rc = ext4_balloc_alloc_block(inode_ref,
					 ext4_fs_inode_to_goal_block(inode_ref),
					 &pblock);
	if (rc != EOK) {
		extent_path_put(fs, path, level + 1);
		return rc;
	}
	inode_ref->dirty = true;

	/*
	 * ⚠️ 关键顺序：**插入之前**必须确认叶子的剩余容量。
	 * 往已满的根叶子里插会写穿 inode 的 60 字节 i_block 区，踩坏后面的
	 * 字段（那是最难查的一类损坏：症状出现在别处）。
	 */
	if (path[level].hdr->eh_entries >= path[level].hdr->eh_max) {
		if (level == 0) {
			/* 根就是叶子且满了：整根下沉，根变成 depth=1 的索引。
			 * 下沉后新节点容量远大于 4，必然放得下。 */
			struct ext4_extent_header *root = path[0].hdr;
			uint8_t saved[EXT4_INODE_BLOCKS * sizeof(uint32_t)];
			struct ext4_block nb;
			struct ext4_extent_header *nh;
			ext4_fsblk_t nphys;

			memcpy(saved, root, sizeof(saved));

			rc = extent_alloc_node(inode_ref, &nb, &nphys, 0);
			if (rc != EOK) {
				ext4_balloc_free_blocks(inode_ref, pblock, 1);
				extent_path_put(fs, path, level + 1);
				return rc;
			}

			nh = (struct ext4_extent_header *)nb.data;
			nh->eh_entries = root->eh_entries;
			memcpy((uint8_t *)nh +
				       sizeof(struct ext4_extent_header),
			       saved + sizeof(struct ext4_extent_header),
			       sizeof(saved) - sizeof(struct ext4_extent_header));
			extent_node_csum(&fs->sb, nb.data);
			ext4_trans_set_block_dirty(nb.buf);
			ext4_block_set(fs->bdev, &nb);

			memset(root, 0, EXT4_INODE_BLOCKS * sizeof(uint32_t));
			root->eh_magic = EXT4_EXTENT_MAGIC;
			root->eh_max = (uint16_t)extent_root_capacity();
			root->eh_depth = 1;
			root->eh_entries = 1;

			struct ext4_extent_idx *ri = extent_idxs(root);
			ri[0].ei_block = 0;
			ri[0].ei_unused = 0;
			extent_idx_set_leaf(&ri[0], nphys);

			inode_ref->dirty = true;
		} else {
			struct ext4_extent_idx up;

			rc = extent_split_node(inode_ref, path[level].hdr, &up);
			if (rc != EOK) {
				ext4_balloc_free_blocks(inode_ref, pblock, 1);
				extent_path_put(fs, path, level + 1);
				return rc;
			}
			if (!path[level].in_inode)
				extent_node_csum(&fs->sb, path[level].raw);

			rc = extent_propagate(inode_ref, path, level, &up);
			if (rc != EOK) {
				ext4_balloc_free_blocks(inode_ref, pblock, 1);
				extent_path_put(fs, path, level + 1);
				return rc;
			}
		}

		/* 结构变了，重新定位叶子（深度最多 5，重走很便宜，
		 * 比追踪"插入点跑去哪了"可靠得多） */
		extent_path_put(fs, path, level + 1);
		level = extent_walk(fs, inode_ref->inode, iblock, path);
		if (level < 0) {
			ext4_balloc_free_blocks(inode_ref, pblock, 1);
			return -level;
		}
		at = extent_leaf_search(path[level].hdr, iblock, &inside);
	}

	/* 此时叶子必有空位 */
	{
		struct ext4_extent_header *leaf = path[level].hdr;
		struct ext4_extent *ents = extent_ents(leaf);
		uint32_t n = leaf->eh_entries;
		bool merged = false;

		/* 与前一 extent 尾部相接 → 直接加长（顺序写的常见情形） */
		if (at >= 0) {
			struct ext4_extent *prev = &ents[at];
			if (!extent_is_uninit(prev) &&
			    (uint64_t)prev->ee_block + extent_len(prev) ==
				    iblock &&
			    extent_start(prev) + extent_len(prev) == pblock) {
				extent_set_len(prev, extent_len(prev) + 1);
				merged = true;
			}
		}
		/* 与后一 extent 头部相接 → 往前扩一格 */
		if (!merged && at + 1 < (int)n) {
			struct ext4_extent *next = &ents[at + 1];
			if (!extent_is_uninit(next) && next->ee_block == iblock + 1 &&
			    extent_start(next) == pblock + 1) {
				next->ee_block = iblock;
				extent_set_len(next, extent_len(next) + 1);
				extent_set_start(next, pblock);
				merged = true;
			}
		}
		if (!merged) {
			int pos = at + 1;
			memmove(&ents[pos + 1], &ents[pos],
				(size_t)(n - (uint32_t)pos) *
					sizeof(struct ext4_extent));
			ents[pos].ee_block = iblock;
			extent_set_len(&ents[pos], 1);
			extent_set_start(&ents[pos], pblock);
			leaf->eh_entries = (uint16_t)(n + 1);
		}

		if (path[level].in_inode)
			inode_ref->dirty = true;
		else {
			extent_node_csum(&fs->sb, path[level].raw);
			ext4_trans_set_block_dirty(path[level].blk.buf);
		}
	}

	*result = pblock;
	if (blocks_count)
		*blocks_count = 1;

	extent_path_put(fs, path, level + 1);
	return EOK;
}

/* ── 对外：remove_space ───────────────────────────────────────────────── */

/*
 * 递归遍历所有叶子，把落在 [from, to) 里的块释放掉，并裁剪相应 extent。
 *
 * 取舍：叶子被清空后**不回收空节点**（树不回缩）。回缩要递归改父节点，
 * 出错就是数据损坏；而空叶子不影响正确性——查找走进去发现没有覆盖项即可。
 * 本内核的使用场景（rootfs + 少量临时文件）几乎不会触发。
 */
static int extent_free_in_node(struct ext4_inode_ref *iref,
			       struct ext4_extent_header *hdr, uint8_t *raw,
			       bool in_inode, ext4_lblk_t from, ext4_lblk_t to)
{
	struct ext4_sblock *sb = &iref->fs->sb;
	int rc = EOK;

	if (hdr->eh_magic != EXT4_EXTENT_MAGIC)
		return EIO;

	if (hdr->eh_depth > 0) {
		struct ext4_extent_idx *idxs = extent_idxs(hdr);

		for (uint32_t k = 0; k < hdr->eh_entries; k++) {
			struct ext4_block child;
			struct ext4_extent_header *ch;

			rc = ext4_trans_block_get(iref->fs->bdev, &child,
						  extent_idx_leaf(&idxs[k]));
			if (rc != EOK)
				return rc;

			ch = (struct ext4_extent_header *)child.data;
			rc = extent_free_in_node(iref, ch, child.data, false,
						 from, to);
			ext4_block_set(iref->fs->bdev, &child);
			if (rc != EOK)
				return rc;
		}
		return EOK;
	}

	/* 叶子：就地裁剪 */
	{
		struct ext4_extent *ents = extent_ents(hdr);
		bool changed = false;
		uint32_t i = 0;

		while (i < hdr->eh_entries) {
			struct ext4_extent *e = &ents[i];
			ext4_lblk_t es = e->ee_block;
			ext4_lblk_t ee = e->ee_block + extent_len(e);
			ext4_lblk_t cs, ce;
			uint32_t drop;
			bool cut_head, cut_tail;

			if (ee <= from) { /* 完全在区间之前 */
				i++;
				continue;
			}
			if (es >= to) /* 完全在区间之后（含之后的全部）*/
				break;

			cs = es > from ? es : from;
			ce = ee < to ? ee : to;
			drop = ce - cs;
			if (drop == 0) {
				i++;
				continue;
			}

			rc = ext4_balloc_free_blocks(
				iref, extent_start(e) + (cs - es), drop);
			if (rc != EOK)
				return rc;
			iref->dirty = true;
			changed = true;

			cut_head = (cs == es);
			cut_tail = (ce == ee);

			if (cut_head && cut_tail) {
				memmove(&ents[i], &ents[i + 1],
					(size_t)(hdr->eh_entries - i - 1) *
						sizeof(struct ext4_extent));
				hdr->eh_entries--;
				continue; /* 同位置换成下一条 */
			}
			if (cut_head) {
				e->ee_block = ce;
				extent_set_start(e, extent_start(e) + drop);
				extent_set_len(e, extent_len(e) - drop);
				i++;
				continue;
			}
			if (cut_tail) {
				extent_set_len(e, extent_len(e) - drop);
				i++;
				continue;
			}
			/* 中间挖洞：拆成前后两条 */
			{
				uint32_t old_len = extent_len(e);
				ext4_fsblk_t old_start = extent_start(e);
				uint32_t tail_len = old_len - (ce - es);

				extent_set_len(e, cs - es);
				memmove(&ents[i + 2], &ents[i + 1],
					(size_t)(hdr->eh_entries - i - 1) *
						sizeof(struct ext4_extent));
				hdr->eh_entries++;
				ents[i + 1].ee_block = ce;
				extent_set_len(&ents[i + 1], tail_len);
				extent_set_start(&ents[i + 1],
						 old_start + (ce - es));
				i += 2;
			}
		}

		if (changed) {
			if (in_inode)
				iref->dirty = true;
			else
				extent_node_csum(sb, raw);
		}
	}
	return rc;
}

int ext4_extent_remove_space(struct ext4_inode_ref *inode_ref, ext4_lblk_t from,
			     ext4_lblk_t to)
{
	struct ext4_extent_header *root =
		ext4_inode_get_extent_header(inode_ref->inode);

	if (from >= to)
		return EOK;
	return extent_free_in_node(inode_ref, root, NULL, true, from, to);
}

#endif /* CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE */
