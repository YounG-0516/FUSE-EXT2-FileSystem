#include "../include/newfs.h"

struct nfs_super      nfs_super; 
struct custom_options nfs_options;

/**
 * @brief 获取文件名
 * 
 * @param path 
 * @return char* 
 */
char* nfs_get_fname(const char* path) {
    char ch = '/';
    // strrchr 函数的作用是在字符串中逆向查找指定字符，并返回该字符最后一次出现的位置的指针。
    char *q = strrchr(path, ch) + 1;   
    // 返回指向斜杠之后的字符的指针 
    return q;
}
/**
 * @brief 计算路径的层级
 * exm: /av/c/d/f
 * -> lvl = 4
 * @param path 
 * @return int 
 */
int nfs_calc_lvl(const char * path) {
    char* str = path;
    int   lvl = 0;
    // 比较给定路径 path 是否与根路径“/”相等。如果相等，则表示路径只有根目录，不包含其他层级，直接返回层级数 lvl。
    if (strcmp(path, "/") == 0) {
        return lvl;
    }
    // 如果指针str指向的字符不是空字符（即路径结尾），则循环判断
    while (*str != NULL) {
        if (*str == '/') {
            lvl++;
        }
        str++;
    }
    return lvl;
}
/**
 * @brief 驱动读
 * 
 * @param offset 
 * @param out_content 
 * @param size 
 * @return int 
 */
int nfs_driver_read(int offset, uint8_t *out_content, int size) {
    // 按照一个逻辑块大小(1024B)封装
    int      offset_aligned = NFS_ROUND_DOWN(offset, NFS_BLK_SZ());
    int      bias           = offset - offset_aligned;
    int      size_aligned   = NFS_ROUND_UP((size + bias), NFS_BLK_SZ());
    uint8_t* temp_content   = (uint8_t*)malloc(size_aligned);
    uint8_t* cur            = temp_content;
    
    // 把磁盘头到down位置
    ddriver_seek(NFS_DRIVER(), offset_aligned, SEEK_SET);

    // 按照磁盘块大小(512B)去读
    while (size_aligned != 0)
    {
        // read(NFS_DRIVER(), cur, NFS_IO_SZ());
        ddriver_read(NFS_DRIVER(), cur, NFS_IO_SZ());
        cur          += NFS_IO_SZ();
        size_aligned -= NFS_IO_SZ();   
    }
    memcpy(out_content, temp_content + bias, size);
    free(temp_content);
    return NFS_ERROR_NONE;
}
/**
 * @brief 驱动写
 * 
 * @param offset 
 * @param in_content 
 * @param size 
 * @return int 
 */
int nfs_driver_write(int offset, uint8_t *in_content, int size) {
    // 按照一个逻辑块大小(1024B)封装
    int      offset_aligned = NFS_ROUND_DOWN(offset, NFS_BLK_SZ());
    int      bias           = offset - offset_aligned;
    int      size_aligned   = NFS_ROUND_UP((size + bias), NFS_BLK_SZ());
    uint8_t* temp_content   = (uint8_t*)malloc(size_aligned);
    uint8_t* cur            = temp_content;
    // 读出需要的磁盘块到内存
    nfs_driver_read(offset_aligned, temp_content, size_aligned);
    // 在内存覆盖指定内容
    memcpy(temp_content + bias, in_content, size);
    
    ddriver_seek(NFS_DRIVER(), offset_aligned, SEEK_SET);

    // 将读出的磁盘块再依次写回到内存
    while (size_aligned != 0)
    {
        ddriver_write(NFS_DRIVER(), cur, NFS_IO_SZ());
        cur          += NFS_IO_SZ();
        size_aligned -= NFS_IO_SZ();   
    }

    free(temp_content);
    return NFS_ERROR_NONE;
}
/**
 * @brief 为一个inode分配dentry，采用头插法
 * 
 * @param inode 
 * @param dentry 
 * @return int 
 */
int nfs_alloc_dentry(struct nfs_inode* inode, struct nfs_dentry* dentry, int judge) {
    if (inode->dentrys == NULL) {
        inode->dentrys = dentry;
    }
    else {
        dentry->brother = inode->dentrys;
        inode->dentrys = dentry;
    }
    inode->dir_cnt++;

    // 如果当前储存dentry的数据块没有存满，在直接采用头插法插入即可
    // 如果当前储存dentry的数据块已存满，则需要开辟新的数据块储存dentry
    int cur_blk = inode->dir_cnt / NFS_DENTRY_PER_DATABLK();    // 第cur_blk个block_pointer还未被使用
    if(judge == 1){
        if(inode->dir_cnt % NFS_DENTRY_PER_DATABLK() == 1){
            /* 在数据块位图上查找空闲的索引节点 */
            int byte_cursor = 0; 
            int bit_cursor  = 0;
            int dno_cursor  = 0;    // 记录data块号
            boolean is_find_free_data_blk = FALSE;
            for (byte_cursor = 0; byte_cursor < NFS_BLKS_SZ(nfs_super.map_data_blks); byte_cursor++)
            {
                for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
                    if((nfs_super.map_data[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                                                            /* 当前dno_cursor位置空闲 */
                        nfs_super.map_data[byte_cursor] |= (0x1 << bit_cursor);

                        inode->block_pointer[cur_blk] = dno_cursor;
                        is_find_free_data_blk = TRUE;
                        break;
                    }
                    dno_cursor++;
                }
                if (is_find_free_data_blk) {
                    break;
                }
            }

            // 未找到空闲数据块
            if (!is_find_free_data_blk || dno_cursor == nfs_super.max_data)
                return -NFS_ERROR_NOSPACE;
        }
    }
    
    return inode->dir_cnt;
}
/**
 * @brief 分配一个inode，占用位图
 * 
 * @param dentry 该dentry指向分配的inode
 * @return nfs_inode
 */
struct nfs_inode* nfs_alloc_inode(struct nfs_dentry * dentry) {
    struct nfs_inode* inode;
    int byte_cursor = 0; 
    int bit_cursor  = 0; 
    int ino_cursor  = 0;    // 记录inode块号
    boolean is_find_free_entry = FALSE;

    // 在索引节点位图上查找空闲的索引节点 
    for (byte_cursor = 0; byte_cursor < NFS_BLKS_SZ(nfs_super.map_inode_blks); byte_cursor++)
    {
        for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
            if((nfs_super.map_inode[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                                                      /* 当前ino_cursor位置空闲 */
                nfs_super.map_inode[byte_cursor] |= (0x1 << bit_cursor);
                is_find_free_entry = TRUE;           
                break;
            }
            ino_cursor++;
        }
        if (is_find_free_entry) {
            break;
        }
    }

    // 未找到空闲结点
    if (!is_find_free_entry || ino_cursor == nfs_super.max_ino)
        return -NFS_ERROR_NOSPACE;

    // 为目录项分配inode节点并建立他们之间的连接
    inode = (struct nfs_inode*)malloc(sizeof(struct nfs_inode));
    inode->ino  = ino_cursor; 
    inode->size = 0;
    inode->dir_cnt = 0;
    inode->dentrys = NULL;

    /* dentry指向inode */
    dentry->inode = inode;
    dentry->ino   = inode->ino;
    /* inode指回dentry */
    inode->dentry = dentry;
    
    /* 如果inode指向文件类型，则需要分配数据指针。如果是目录则不需要，目录项已存在dentrys中*/
    if (NFS_IS_REG(inode)) {
        for(int i=0; i<NFS_DATA_PER_FILE; i++){
            inode->data[i] = (uint8_t *)malloc(NFS_BLK_SZ());
        }
    }

    return inode;
}
/**
 * @brief 将内存inode及其下方结构全部刷回磁盘
 * 
 * @param inode 
 * @return int 
 */
int nfs_sync_inode(struct nfs_inode * inode) {
    struct nfs_inode_d  inode_d;
    struct nfs_dentry*  dentry_cursor;
    struct nfs_dentry_d dentry_d;
    int offset;
    int ino             = inode->ino;

    /* 根据内存中的inode刷回磁盘的inode_d */
    inode_d.ino         = ino;
    inode_d.size        = inode->size;
    inode_d.ftype       = inode->dentry->ftype;
    inode_d.dir_cnt     = inode->dir_cnt;
    for(int i=0; i<NFS_DATA_PER_FILE; i++){
        inode_d.block_pointer[i] = inode->block_pointer[i];
    }
    
    /* 将inode_d刷回磁盘 */
    if (nfs_driver_write(NFS_INO_OFS(ino), (uint8_t *)&inode_d, 
                     sizeof(struct nfs_inode_d)) != NFS_ERROR_NONE) {
        NFS_DBG("[%s] io error\n", __func__);
        return -NFS_ERROR_IO;
    }

    /* Cycle 1: 写 INODE */
    /* Cycle 2: 写 数据 */
    /* 如果是目录类型则需要首先将目录项写入磁盘，再递归刷写每一个目录项所对应的inode节点 */
    if (NFS_IS_DIR(inode)) {    
        
        int blk_number = 0;                      
        dentry_cursor = inode->dentrys;

        /* 将dentry写回4个磁盘块*/
        while(dentry_cursor != NULL && blk_number < NFS_DATA_PER_FILE){
            offset = NFS_DATA_OFS(inode->block_pointer[blk_number]);
            while ((dentry_cursor != NULL) && (offset < NFS_DATA_OFS(inode->block_pointer[blk_number] + 1)))
            {
                /* 用内存中dentry_cursor指向的dentry更新将要刷回磁盘的dentry_d */
                memcpy(dentry_d.fname, dentry_cursor->fname, NFS_MAX_FILE_NAME);
                dentry_d.ftype = dentry_cursor->ftype;
                dentry_d.ino = dentry_cursor->ino;
                if (nfs_driver_write(offset, (uint8_t *)&dentry_d, sizeof(struct nfs_dentry_d)) != NFS_ERROR_NONE) {
                    NFS_DBG("[%s] io error\n", __func__);
                    return -NFS_ERROR_IO;                     
                }
                
                /* 递归调用 */
                if (dentry_cursor->inode != NULL) {
                    nfs_sync_inode(dentry_cursor->inode);
                }

                dentry_cursor = dentry_cursor->brother;
                offset += sizeof(struct nfs_dentry_d);
            }
            blk_number++;
        }
    }
    /* 如果是文件类型，则将inode所指向的数据直接写入磁盘 */
    else if (NFS_IS_REG(inode)) {
        for(int i=0; i<NFS_DATA_PER_FILE; i++){
            if (nfs_driver_write(NFS_DATA_OFS(inode->block_pointer[i]), inode->data[i], NFS_BLK_SZ()) != NFS_ERROR_NONE) {
                NFS_DBG("[%s] io error\n", __func__);
                return -NFS_ERROR_IO;
            }
        }
    }
    return NFS_ERROR_NONE;
}
/**
 * @brief 
 * 
 * @param dentry dentry指向ino，读取该inode
 * @param ino inode唯一编号
 * @return struct nfs_inode* 
 */
struct nfs_inode* nfs_read_inode(struct nfs_dentry * dentry, int ino) {
    struct nfs_inode* inode = (struct nfs_inode*)malloc(sizeof(struct nfs_inode));
    struct nfs_inode_d inode_d;
    struct nfs_dentry* sub_dentry;
    struct nfs_dentry_d dentry_d;
    int    dir_cnt = 0;

    /* 从磁盘中读取ino对应的inode_d */
    if (nfs_driver_read(NFS_INO_OFS(ino), (uint8_t *)&inode_d, 
                        sizeof(struct nfs_inode_d)) != NFS_ERROR_NONE) {
        NFS_DBG("[%s] io error\n", __func__);
        return NULL;                    
    }

    /* 根据inode_d更新内存中inode参数 */
    inode->dir_cnt = 0;
    inode->ino = inode_d.ino;
    inode->size = inode_d.size;
    inode->dentry = dentry;
    inode->dentrys = NULL;
    for(int i = 0; i < NFS_DATA_PER_FILE; i++){
        inode->block_pointer[i] = inode_d.block_pointer[i];
    }

    /*判断iNode节点的文件类型*/
    /*如果inode是目录类型，则需要读取每一个目录项并建立连接*/
    if (NFS_IS_DIR(inode)) {
        dir_cnt = inode_d.dir_cnt;
        int blk_number = 0;
        int offset;

        /*处理每一个目录项*/
        while(dir_cnt > 0 && blk_number < NFS_DATA_PER_FILE){
            offset = NFS_DATA_OFS(inode->block_pointer[blk_number]);

            // 当从磁盘读入时，由于磁盘中没有链表指针，因此只能通过一个dentry_d大小来进行遍历
            while((dir_cnt > 0) && (offset + sizeof(struct nfs_dentry_d) < NFS_DATA_OFS(inode->block_pointer[blk_number] + 1))){
                if (nfs_driver_read(offset, (uint8_t *)&dentry_d, sizeof(struct nfs_dentry_d)) != NFS_ERROR_NONE){
                    NFS_DBG("[%s] io error\n", __func__);
                    return NULL;  
                }
                
                /* 用从磁盘中读出的dentry_d更新内存中的sub_dentry */
                sub_dentry = new_dentry(dentry_d.fname, dentry_d.ftype);
                sub_dentry->parent = inode->dentry;
                sub_dentry->ino    = dentry_d.ino; 
                nfs_alloc_dentry(inode, sub_dentry, 0);

                offset += sizeof(struct nfs_dentry_d);
                dir_cnt--;
            }
            blk_number++;
        }
    }
    /*如果inode是文件类型，则直接读取数据即可*/
    else if (NFS_IS_REG(inode)) {
        for (int i = 0; i < NFS_DATA_PER_FILE; i++){
            inode->data[i] = (uint8_t *)malloc(NFS_BLK_SZ());
            if (nfs_driver_read(NFS_DATA_OFS(inode->block_pointer[i]), (uint8_t *)inode->data[i], 
                                NFS_BLK_SZ()) != NFS_ERROR_NONE) {
                NFS_DBG("[%s] io error\n", __func__);
                return NULL;                    
            }
        }
    }

    return inode;
}
/**
 * @brief 
 * 
 * @param inode 
 * @param dir [0...]
 * @return struct nfs_dentry* 
 */
struct nfs_dentry* nfs_get_dentry(struct nfs_inode * inode, int dir) {
    struct nfs_dentry* dentry_cursor = inode->dentrys;
    int    cnt = 0;
    while (dentry_cursor)
    {
        if (dir == cnt) {
            return dentry_cursor;
        }
        cnt++;
        dentry_cursor = dentry_cursor->brother;
    }
    return NULL;
}
/**
 * @brief 找到路径所对应的目录项，或者返回上一级目录项
 * path: /qwe/ad  total_lvl = 2,
 *      1) find /'s inode       lvl = 1
 *      2) find qwe's dentry 
 *      3) find qwe's inode     lvl = 2
 *      4) find ad's dentry
 *
 * path: /qwe     total_lvl = 1,
 *      1) find /'s inode       lvl = 1
 *      2) find qwe's dentry
 * 
 * @param path 
 * @return struct nfs_inode* 
 */
struct nfs_dentry* nfs_lookup(const char * path, boolean* is_find, boolean* is_root) {
    struct nfs_dentry* dentry_cursor = nfs_super.root_dentry;
    struct nfs_dentry* dentry_ret = NULL;
    struct nfs_inode*  inode; 
    int   total_lvl = nfs_calc_lvl(path);
    int   lvl = 0;
    boolean is_hit;
    char* fname = NULL;
    char* path_cpy = (char*)malloc(sizeof(path));
    *is_root = FALSE;
    strcpy(path_cpy, path);

    /* 如果路径的级数为0，则说明是根目录，直接返回根目录项即可 */
    if (total_lvl == 0) {                           
        *is_find = TRUE;
        *is_root = TRUE;
        dentry_ret = nfs_super.root_dentry;
    }

    /* 获取最外层文件夹名称 */
    fname = strtok(path_cpy, "/");       
    while (fname)
    {   
        lvl++;

        /* Cache机制，如果当前dentry对应的inode为空，则从磁盘中读取 */
        if (dentry_cursor->inode == NULL) {           
            nfs_read_inode(dentry_cursor, dentry_cursor->ino);
        }

        /* 当前dentry对应的inode */
        inode = dentry_cursor->inode;

        /* 若当前inode对应文件类型，且还没查找到对应层数，说明路径错误，跳出循环 */
        if (NFS_IS_REG(inode) && lvl < total_lvl) {
            NFS_DBG("[%s] not a dir\n", __func__);
            dentry_ret = inode->dentry;
            break;
        }
        /* 若当前inode对应文件夹 */
        if (NFS_IS_DIR(inode)) {
            dentry_cursor = inode->dentrys;
            is_hit        = FALSE;

            /* 遍历该目录下所有目录项 */
            while (dentry_cursor)
            {
                /* 如果名称匹配，则命中 */
                if (memcmp(dentry_cursor->fname, fname, strlen(fname)) == 0) {
                    is_hit = TRUE;
                    break;
                }
                // 否则查找下一个目录项
                dentry_cursor = dentry_cursor->brother;
            }
            
            /* 若未命中 */
            if (!is_hit) {
                *is_find = FALSE;
                NFS_DBG("[%s] not found %s\n", __func__, fname);
                dentry_ret = inode->dentry;
                break;
            }

            /* 若命中 */
            if (is_hit && lvl == total_lvl) {
                *is_find = TRUE;
                dentry_ret = dentry_cursor;
                break;
            }
        }
        /* 获取下一层文件夹名称 */
        fname = strtok(NULL, "/"); 
    }

    /* 如果对应dentry的inode还没读进来，则重新读 */
    if (dentry_ret->inode == NULL) {
        dentry_ret->inode = nfs_read_inode(dentry_ret, dentry_ret->ino);
    }
    
    return dentry_ret;
}
/**
 * @brief 挂载nfs, 
 * 16个Inode占用一个Blk
 * @param options 
 * @return int 
 */
int nfs_mount(struct custom_options options){
    int                 ret = NFS_ERROR_NONE;
    int                 driver_fd;
    struct nfs_super_d  nfs_super_d; 
    struct nfs_dentry*  root_dentry;
    struct nfs_inode*   root_inode;

    int                 inode_num;
    int                 map_inode_blks;

    int                 data_num;
    int                 map_data_blks;
    
    int                 super_blks;
    boolean             is_init = FALSE;

    nfs_super.is_mounted = FALSE;

    driver_fd = ddriver_open(options.device);

    if (driver_fd < 0) {
        return driver_fd;
    }

    // 向超级块中写入相关信息
    nfs_super.fd = driver_fd;
    ddriver_ioctl(NFS_DRIVER(), IOC_REQ_DEVICE_SIZE,  &nfs_super.sz_disk);
    ddriver_ioctl(NFS_DRIVER(), IOC_REQ_DEVICE_IO_SZ, &nfs_super.sz_io);
    nfs_super.sz_blks = 2 * nfs_super.sz_io;  
    
    // 创建根目录
    root_dentry = new_dentry("/", NFS_DIR);

    // 读取磁盘超级块nfs_super_d到内存。
    if (nfs_driver_read(NFS_SUPER_OFS, (uint8_t *)(&nfs_super_d), sizeof(struct nfs_super_d)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }   
    
    // 根据超级块幻数判断是否为第一次启动磁盘，如果是第一次启动磁盘，则需要建立磁盘超级块的布局。
    // 读取super
    if (nfs_super_d.magic_num != NFS_MAGIC_NUM) {     /* 幻数无 */
        /* 估算各部分大小 */
        /* 计算过程位于types.h中 */
        super_blks = NFS_SUPER_BLKS;
        inode_num  = NFS_INODE_BLKS;
        data_num = NFS_DATA_BLKS;
        map_inode_blks = NFS_MAP_INODE_BLKS;
        map_data_blks = NFS_MAP_DATA_BLKS;

        /* 布局layout */
        nfs_super.max_ino = inode_num;
        nfs_super.max_data = data_num;

        nfs_super_d.map_inode_blks = map_inode_blks; 
        nfs_super_d.map_data_blks = map_data_blks; 

        nfs_super_d.map_inode_offset = NFS_SUPER_OFS + NFS_BLKS_SZ(super_blks);
        nfs_super_d.map_data_offset = nfs_super_d.map_inode_offset + NFS_BLKS_SZ(map_inode_blks);

        nfs_super_d.inode_offset = nfs_super_d.map_data_offset + NFS_BLKS_SZ(map_data_blks);
        nfs_super_d.data_offset = nfs_super_d.inode_offset + NFS_BLKS_SZ(inode_num);

        nfs_super_d.sz_usage = 0;
        nfs_super_d.magic_num = NFS_MAGIC_NUM;

        is_init = TRUE;
    }

    /* 建立 in-memory 结构 */
    // 初始化超级块
    nfs_super.sz_usage   = nfs_super_d.sz_usage; 

    // 建立inode位图    
    nfs_super.map_inode = (uint8_t *)malloc(NFS_BLKS_SZ(nfs_super_d.map_inode_blks));
    nfs_super.map_inode_blks = nfs_super_d.map_inode_blks;
    nfs_super.map_inode_offset = nfs_super_d.map_inode_offset;
    nfs_super.inode_offset = nfs_super_d.inode_offset;

    // 建立数据块位图
    nfs_super.map_data = (uint8_t *)malloc(NFS_BLKS_SZ(nfs_super_d.map_data_blks));
    nfs_super.map_data_blks = nfs_super_d.map_data_blks;
    nfs_super.map_data_offset = nfs_super_d.map_data_offset;
    nfs_super.data_offset = nfs_super_d.data_offset;

    if (nfs_driver_read(nfs_super_d.map_inode_offset, (uint8_t *)(nfs_super.map_inode), 
                        NFS_BLKS_SZ(nfs_super_d.map_inode_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    if (nfs_driver_read(nfs_super_d.map_data_offset, (uint8_t *)(nfs_super.map_data), 
                        NFS_BLKS_SZ(nfs_super_d.map_data_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    if (is_init) {                                    /* 分配根节点 */
        root_inode = nfs_alloc_inode(root_dentry);
        nfs_sync_inode(root_inode);
    }
    
    root_inode            = nfs_read_inode(root_dentry, NFS_ROOT_INO);
    root_dentry->inode    = root_inode;
    nfs_super.root_dentry = root_dentry;
    nfs_super.is_mounted  = TRUE;

    return ret;
}
/**
 * @brief 卸载nfs
 * 
 * @return int 
 */
int nfs_umount() {
    struct nfs_super_d  nfs_super_d; 

    if (!nfs_super.is_mounted) {
        return NFS_ERROR_NONE;
    }

    nfs_sync_inode(nfs_super.root_dentry->inode);     /* 从根节点向下刷写节点 */

    /* 用内存中的超级块nfs_super更新即将写回磁盘的nfs_super_d */                                    
    nfs_super_d.magic_num           = NFS_MAGIC_NUM;
    nfs_super_d.sz_usage            = nfs_super.sz_usage;

    nfs_super_d.map_inode_blks      = nfs_super.map_inode_blks;
    nfs_super_d.map_inode_offset    = nfs_super.map_inode_offset;
    nfs_super_d.inode_offset        = nfs_super.inode_offset;

    nfs_super_d.map_data_blks       = nfs_super.map_data_blks;
    nfs_super_d.map_data_offset     = nfs_super.map_data_offset;
    nfs_super_d.data_offset         = nfs_super.data_offset;
    
    /* 将超级块刷回磁盘 */
    if (nfs_driver_write(NFS_SUPER_OFS, (uint8_t *)&nfs_super_d, 
                     sizeof(struct nfs_super_d)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    /* 将inode位图刷回磁盘 */
    if (nfs_driver_write(nfs_super_d.map_inode_offset, (uint8_t *)(nfs_super.map_inode), 
                         NFS_BLKS_SZ(nfs_super_d.map_inode_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    /* 将data位图刷回磁盘 */
    if (nfs_driver_write(nfs_super_d.map_data_offset, (uint8_t *)(nfs_super.map_data), 
                         NFS_BLKS_SZ(nfs_super_d.map_data_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    /* 释放内存中的inode位图和data位图 */
    free(nfs_super.map_inode);
    free(nfs_super.map_data);

    /* 关闭驱动 */
    ddriver_close(NFS_DRIVER());

    return NFS_ERROR_NONE;
}
