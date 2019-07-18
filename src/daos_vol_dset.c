/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of the HDF5 DAOS VOL connector. The full copyright      *
 * notice, including terms governing use, modification, and redistribution,  *
 * is contained in the COPYING file, which can be found at the root of the   *
 * source code distribution tree.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: The DAOS VOL connector where access is forwarded to the DAOS
 * library. Dataset routines.
 */

#include "daos_vol.h"           /* DAOS connector                          */

#include "util/daos_vol_err.h"  /* DAOS connector error handling           */
#include "util/daos_vol_mem.h"  /* DAOS connector memory management        */

/* Macros */
/* Definitions for chunking code */
#define H5_DAOS_DEFAULT_NUM_SEL_CHUNKS   64
#define H5O_LAYOUT_NDIMS                 (H5S_MAX_RANK+1)

/* Typedefs */
/* Udata type for H5Dscatter callback */
typedef struct H5_daos_scatter_cb_ud_t {
    void *buf;
    size_t len;
} H5_daos_scatter_cb_ud_t;

/* Udata type for memory space H5Diterate callback */
typedef struct {
    daos_iod_t *iods;
    daos_sg_list_t *sgls;
    daos_iov_t *sg_iovs;
    hbool_t is_vl_str;
    size_t base_type_size;
    uint64_t offset;
    uint64_t idx;
} H5_daos_vl_mem_ud_t;

/* Udata type for file space H5Diterate callback */
typedef struct {
    uint8_t **akeys;
    daos_iod_t *iods;
    uint64_t idx;
} H5_daos_vl_file_ud_t;

/* Information about a singular selected chunk during a dataset read/write */
typedef struct H5_daos_select_chunk_info_t {
    uint64_t chunk_coords[H5S_MAX_RANK]; /* The starting coordinates ("upper left corner") of the chunk */
    hid_t    mspace_id;                  /* The memory space corresponding to the
                                            selection in the chunk in memory */
    hid_t    fspace_id;                  /* The file space corresponding to the
                                            selection in the chunk in the file */
} H5_daos_select_chunk_info_t;

/* Enum type for distinguishing between dataset reads and writes. */
typedef enum dset_io_type {
    IO_READ,
    IO_WRITE
} dset_io_type;

/* Typedef for function to perform I/O on a single chunk */
typedef herr_t (*H5_daos_chunk_io_func)(H5_daos_dset_t *dset, daos_key_t dkey,
    hssize_t num_elem, hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id,
    hid_t dxpl_id, dset_io_type io_type, void *buf);

/* Prototypes */
static herr_t H5_daos_sel_to_recx_iov(hid_t space_id, size_t type_size,
    void *buf, daos_recx_t **recxs, daos_iov_t **sg_iovs, size_t *list_nused);
static herr_t H5_daos_scatter_cb(const void **src_buf,
    size_t *src_buf_bytes_used, void *_udata);
static herr_t H5_daos_dataset_mem_vl_rd_cb(void *_elem, hid_t type_id,
    unsigned ndim, const hsize_t *point, void *_udata);
static herr_t H5_daos_dataset_file_vl_cb(void *_elem, hid_t type_id,
    unsigned ndim, const hsize_t *point, void *_udata);
static herr_t H5_daos_dataset_mem_vl_wr_cb(void *_elem, hid_t type_id,
    unsigned ndim, const hsize_t *point, void *_udata);
static herr_t H5_daos_dataset_io_vl(H5_daos_dset_t *dset, daos_key_t dkey,
    hssize_t num_elem, hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id,
    hid_t dxpl_id, dset_io_type io_type, void *buf);
static herr_t H5_daos_dataset_io_types_equal(H5_daos_dset_t *dset, daos_key_t dkey,
    hssize_t num_elem, hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id,
    hid_t dxpl_id, dset_io_type io_type, void *buf);
static herr_t H5_daos_dataset_io_types_unequal(H5_daos_dset_t *dset, daos_key_t dkey,
    hssize_t num_elem, hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id,
    hid_t dxpl_id, dset_io_type io_type, void *buf);
static herr_t H5_daos_dataset_set_extent(H5_daos_dset_t *dset,
    const hsize_t *size, hid_t dxpl_id, void **req);
static herr_t H5_daos_get_selected_chunk_info(hid_t dcpl_id,
    hid_t file_space_id, hid_t mem_space_id,
    H5_daos_select_chunk_info_t **chunk_info, size_t *chunk_info_len);
static hbool_t H5_daos_is_partial_edge_chunk(unsigned dims_rank,
    const hsize_t *dset_dims, const hsize_t *chunk_dims, const hsize_t *chunk_coords);


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_create
 *
 * Purpose:     Sends a request to DAOS to create a dataset
 *
 * Return:      Success:        dataset object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
void *
H5_daos_dataset_create(void *_item,
    const H5VL_loc_params_t H5VL_DAOS_UNUSED *loc_params, const char *name,
    hid_t H5VL_DAOS_UNUSED lcpl_id, hid_t type_id, hid_t space_id, hid_t dcpl_id,
    hid_t dapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_dset_t *dset = NULL;
    H5_daos_group_t *target_grp = NULL;
    void *type_buf = NULL;
    void *space_buf = NULL;
    void *dcpl_buf = NULL;
    hbool_t collective;
    tse_task_t *finalize_task;
    int finalize_ndeps = 0;
    tse_task_t *finalize_deps[2];
    H5_daos_req_t *int_req = NULL;
    int ret;
    void *ret_value = NULL;

    if(!_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "dataset parent object is NULL")
    if(!loc_params)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "location parameters object is NULL")

    /* Check for write access */
    if(!(item->file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, NULL, "no write intent on file")

    /*
     * Like HDF5, all metadata writes are collective by default. Once independent
     * metadata writes are implemented, we will need to check for this property.
     */
    collective = TRUE;

    /* Start H5 operation */
    if(NULL == (int_req = (H5_daos_req_t *)DV_malloc(sizeof(H5_daos_req_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for request")
    int_req->th = DAOS_TX_NONE;
    int_req->th_open = FALSE;
    int_req->file = item->file;
    int_req->file->item.rc++;
    int_req->rc = 1;
    int_req->status = H5_DAOS_INCOMPLETE;
    int_req->failed_task = NULL;

    /* Allocate the dataset object that is returned to the user */
    if(NULL == (dset = H5FL_CALLOC(H5_daos_dset_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    dset->obj.item.type = H5I_DATASET;
    dset->obj.item.open_req = int_req;
    int_req->rc++;
    dset->obj.item.file = item->file;
    dset->obj.item.rc = 1;
    dset->obj.obj_oh = DAOS_HDL_INVAL;
    dset->type_id = FAIL;
    dset->space_id = FAIL;
    dset->dcpl_id = FAIL;
    dset->dapl_id = FAIL;

    /* Generate dataset oid */
    H5_daos_oid_encode(&dset->obj.oid, item->file->max_oid + (uint64_t)1, H5I_DATASET);

    /* Create dataset and write metadata if this process should */
    if(!collective || (item->file->my_rank == 0)) {
        const char *target_name = NULL;
        daos_key_t dkey;
        daos_iod_t iod[3];
        daos_sg_list_t sgl[3];
        daos_iov_t sg_iov[3];
        size_t type_size = 0;
        size_t space_size = 0;
        size_t dcpl_size = 0;
        tse_task_t *link_write_task;

        /* Traverse the path */
        if(name)
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, NULL, "can't traverse path")

        /* Create dataset */
        /* Update max_oid */
        item->file->max_oid = H5_daos_oid_to_idx(dset->obj.oid);

        /* Write max OID */
        if(H5_daos_write_max_oid(item->file) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't write max OID")

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, DAOS_OO_RW, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %s", H5_daos_err_to_string(ret))

        /* Encode datatype */
        if(H5Tencode(type_id, NULL, &type_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of datatype")
        if(NULL == (type_buf = DV_malloc(type_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
        if(H5Tencode(type_id, type_buf, &type_size) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize datatype")

        /* Encode dataspace */
        if(H5Sencode2(space_id, NULL, &space_size, item->file->fapl_id) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of dataspace")
        if(NULL == (space_buf = DV_malloc(space_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized dataspace")
        if(H5Sencode2(space_id, space_buf, &space_size, item->file->fapl_id) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize dataspace")

        /* Encode DCPL */
        if(H5Pencode2(dcpl_id, NULL, &dcpl_size, item->file->fapl_id) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of dcpl")
        if(NULL == (dcpl_buf = DV_malloc(dcpl_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized dcpl")
        if(H5Pencode2(dcpl_id, dcpl_buf, &dcpl_size, item->file->fapl_id) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize dcpl")

        /* Set up operation to write datatype, dataspace, and DCPL to dataset */
        /* Set up dkey */
        daos_iov_set(&dkey, H5_daos_int_md_key_g, H5_daos_int_md_key_size_g);

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, H5_daos_type_key_g, H5_daos_type_key_size_g);
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = (uint64_t)type_size;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, H5_daos_space_key_g, H5_daos_space_key_size_g);
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = (uint64_t)space_size;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[2].iod_name, H5_daos_cpl_key_g, H5_daos_cpl_key_size_g);
        daos_csum_set(&iod[2].iod_kcsum, NULL, 0);
        iod[2].iod_nr = 1u;
        iod[2].iod_size = (uint64_t)dcpl_size;
        iod[2].iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov[0], type_buf, (daos_size_t)type_size);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        daos_iov_set(&sg_iov[1], space_buf, (daos_size_t)space_size);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];
        daos_iov_set(&sg_iov[2], dcpl_buf, (daos_size_t)dcpl_size);
        sgl[2].sg_nr = 1;
        sgl[2].sg_nr_out = 0;
        sgl[2].sg_iovs = &sg_iov[2];

        /* Write internal metadata to dataset */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 3, iod, sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't write metadata to dataset: %s", H5_daos_err_to_string(ret))

        /* Create link to dataset */
        if(target_grp) {
            H5_daos_link_val_t link_val;

            link_val.type = H5L_TYPE_HARD;
            link_val.target.hard = dset->obj.oid;
            if(H5_daos_link_write(target_grp, target_name, strlen(target_name), &link_val, int_req, &link_write_task) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't create link to dataset")
            finalize_deps[finalize_ndeps] = link_write_task;
            finalize_ndeps++;
        } /* end if */
    } /* end if */
    else {
        /* Update max_oid */
        item->file->max_oid = dset->obj.oid.lo;

        /* Note no barrier is currently needed here, daos_obj_open is a local
         * operation and can occur before the lead process writes metadata.  For
         * app-level synchronization we could add a barrier or bcast though it
         * could only be an issue with dataset reopen so we'll skip it for now.
         * There is probably never an issue with file reopen since all commits
         * are from process 0, same as the dataset create above. */

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, DAOS_OO_RW, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %s", H5_daos_err_to_string(ret))
    } /* end else */

    /* Finish setting up dataset struct */
    if((dset->type_id = H5Tcopy(type_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTCOPY, NULL, "failed to copy datatype")
    if((dset->space_id = H5Scopy(space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTCOPY, NULL, "failed to copy dataspace")
    if(H5Sselect_all(dset->space_id) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTDELETE, NULL, "can't change selection")
    if((dset->dcpl_id = H5Pcopy(dcpl_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTCOPY, NULL, "failed to copy dcpl")
    if((dset->dapl_id = H5Pcopy(dapl_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTCOPY, NULL, "failed to copy dapl")

    /* Set return value */
    ret_value = (void *)dset;

done:
    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, NULL) < 0)
        D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close group")

    if(int_req) {
        /* Create task to finalize H5 operation */
        if(0 != (ret = tse_task_create(H5_daos_h5op_finalize, &item->file->sched, int_req, &finalize_task)))
            D_DONE_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't create task to finalize H5 operation: %s", H5_daos_err_to_string(ret))
        /* Register dependencies (if any) */
        else if(finalize_ndeps > 0 && 0 != (ret = tse_task_register_deps(finalize_task, finalize_ndeps, finalize_deps)))
            D_DONE_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't create dependencies for task to finalize H5 operation: %s", H5_daos_err_to_string(ret))
        /* Schedule finalize task */
        else if(0 != (ret = tse_task_schedule(finalize_task, false)))
            D_DONE_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't schedule task to finalize H5 operation: %s", H5_daos_err_to_string(ret))
        else
            /* finalize_task now owns a reference to req */
            int_req->rc++;

        /* Block until operation completes */
        {
            bool is_empty;

            /* Wait for scheduler to be empty *//* Change to custom progress function DSINC */
            if(0 != (ret = daos_progress(&item->file->sched, DAOS_EQ_WAIT, &is_empty)))
                D_DONE_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't progress scheduler: %s", H5_daos_err_to_string(ret))

            /* Check for failure */
            if(int_req->status < 0)
                D_DONE_ERROR(H5E_DATASET, H5E_CANTOPERATE, NULL, "dataset creation failed in task \"%s\": %s", int_req->failed_task, H5_daos_err_to_string(int_req->status))
        } /* end block */

        /* Close internal request */
        H5_daos_req_free_int(int_req);
    } /* end if */

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close dataset */
        if(dset && H5_daos_dataset_close(dset, dxpl_id, NULL) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close dataset")

    /* Free memory */
    type_buf = DV_free(type_buf);
    space_buf = DV_free(space_buf);
    dcpl_buf = DV_free(dcpl_buf);

    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_open
 *
 * Purpose:     Sends a request to DAOS to open a dataset
 *
 * Return:      Success:        dataset object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
void *
H5_daos_dataset_open(void *_item,
    const H5VL_loc_params_t H5VL_DAOS_UNUSED *loc_params, const char *name,
    hid_t dapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_dset_t *dset = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    daos_key_t dkey;
    daos_iod_t iod[3];
    daos_sg_list_t sgl[3];
    daos_iov_t sg_iov[3];
    uint64_t type_len = 0;
    uint64_t space_len = 0;
    uint64_t dcpl_len = 0;
    uint64_t tot_len;
    uint8_t dinfo_buf_static[H5_DAOS_DINFO_BUF_SIZE];
    uint8_t *dinfo_buf_dyn = NULL;
    uint8_t *dinfo_buf = dinfo_buf_static;
    uint8_t *p;
    hbool_t collective;
    hbool_t must_bcast = FALSE;
    int ret;
    void *ret_value = NULL;

    if(!_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "dataset parent object is NULL")
    if(!loc_params)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "location parameters object is NULL")

    /*
     * Like HDF5, metadata reads are independent by default. If the application has specifically
     * requested collective metadata reads, they will be enabled here.
     */
    collective = item->file->is_collective_md_read;
    if(!collective && (H5P_DATASET_ACCESS_DEFAULT != dapl_id))
        if(H5Pget_all_coll_metadata_ops(dapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, NULL, "can't get collective metadata reads property")

    /* Allocate the dataset object that is returned to the user */
    if(NULL == (dset = H5FL_CALLOC(H5_daos_dset_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    dset->obj.item.type = H5I_DATASET;
    dset->obj.item.open_req = NULL;
    dset->obj.item.file = item->file;
    dset->obj.item.rc = 1;
    dset->obj.obj_oh = DAOS_HDL_INVAL;
    dset->type_id = FAIL;
    dset->space_id = FAIL;
    dset->dcpl_id = FAIL;
    dset->dapl_id = FAIL;

    /* Check if we're actually opening the group or just receiving the dataset
     * info from the leader */
    if(!collective || (item->file->my_rank == 0)) {
        if(collective && (item->file->num_procs > 1))
            must_bcast = TRUE;

        /* Check for open by address */
        if(H5VL_OBJECT_BY_ADDR == loc_params->type) {
            /* Generate oid from address */
            H5_daos_oid_generate(&dset->obj.oid, (uint64_t)loc_params->loc_data.loc_by_addr.addr, H5I_DATASET);
        } /* end if */
        else {
            /* Open using name parameter */
            if(H5VL_OBJECT_BY_SELF != loc_params->type)
                D_GOTO_ERROR(H5E_ARGS, H5E_UNSUPPORTED, NULL, "unsupported dataset open location parameters type")
            if(!name)
                D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "dataset name is NULL")

            /* Traverse the path */
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, NULL, "can't traverse path")

            /* Follow link to dataset */
            if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, &dset->obj.oid) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't follow link to dataset")
        } /* end else */

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %s", H5_daos_err_to_string(ret))

        /* Set up operation to read datatype, dataspace, and DCPL sizes from
         * dataset */
        /* Set up dkey */
        daos_iov_set(&dkey, H5_daos_int_md_key_g, H5_daos_int_md_key_size_g);

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, H5_daos_type_key_g, H5_daos_type_key_size_g);
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = DAOS_REC_ANY;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, H5_daos_space_key_g, H5_daos_space_key_size_g);
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = DAOS_REC_ANY;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[2].iod_name, (void *)H5_daos_cpl_key_g, H5_daos_cpl_key_size_g);
        daos_csum_set(&iod[2].iod_kcsum, NULL, 0);
        iod[2].iod_nr = 1u;
        iod[2].iod_size = DAOS_REC_ANY;
        iod[2].iod_type = DAOS_IOD_SINGLE;

        /* Read internal metadata sizes from dataset */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 3, iod, NULL,
                      NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTDECODE, NULL, "can't read metadata sizes from dataset: %s", H5_daos_err_to_string(ret))

        /* Check for metadata not found */
        if((iod[0].iod_size == (uint64_t)0) || (iod[1].iod_size == (uint64_t)0)
                || (iod[2].iod_size == (uint64_t)0))
            D_GOTO_ERROR(H5E_DATASET, H5E_NOTFOUND, NULL, "internal metadata not found")

        /* Compute dataset info buffer size */
        type_len = iod[0].iod_size;
        space_len = iod[1].iod_size;
        dcpl_len = iod[2].iod_size;
        tot_len = type_len + space_len + dcpl_len;

        /* Allocate dataset info buffer if necessary */
        if((tot_len + (5 * sizeof(uint64_t))) > sizeof(dinfo_buf_static)) {
            if(NULL == (dinfo_buf_dyn = (uint8_t *)DV_malloc(tot_len + (5 * sizeof(uint64_t)))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate dataset info buffer")
            dinfo_buf = dinfo_buf_dyn;
        } /* end if */

        /* Set up sgl */
        p = dinfo_buf + (5 * sizeof(uint64_t));
        daos_iov_set(&sg_iov[0], p, (daos_size_t)type_len);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        p += type_len;
        daos_iov_set(&sg_iov[1], p, (daos_size_t)space_len);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];
        p += space_len;
        daos_iov_set(&sg_iov[2], p, (daos_size_t)dcpl_len);
        sgl[2].sg_nr = 1;
        sgl[2].sg_nr_out = 0;
        sgl[2].sg_iovs = &sg_iov[2];

        /* Read internal metadata from dataset */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 3, iod, sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTDECODE, NULL, "can't read metadata from dataset: %s", H5_daos_err_to_string(ret))

        /* Broadcast dataset info if there are other processes that need it */
        if(collective && (item->file->num_procs > 1)) {
            assert(dinfo_buf);
            assert(sizeof(dinfo_buf_static) >= 5 * sizeof(uint64_t));

            /* Encode oid */
            p = dinfo_buf;
            UINT64ENCODE(p, dset->obj.oid.lo)
            UINT64ENCODE(p, dset->obj.oid.hi)

            /* Encode serialized info lengths */
            UINT64ENCODE(p, type_len)
            UINT64ENCODE(p, space_len)
            UINT64ENCODE(p, dcpl_len)

            /* MPI_Bcast dinfo_buf */
            if(MPI_SUCCESS != MPI_Bcast((char *)dinfo_buf, sizeof(dinfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't broadcast dataset info")

            /* Need a second bcast if it did not fit in the receivers' static
             * buffer */
            if(tot_len + (5 * sizeof(uint64_t)) > sizeof(dinfo_buf_static))
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                    D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't broadcast dataset info (second broadcast)")
        } /* end if */
        else
            p = dinfo_buf + (5 * sizeof(uint64_t));
    } /* end if */
    else {
        /* Receive dataset info */
        if(MPI_SUCCESS != MPI_Bcast((char *)dinfo_buf, sizeof(dinfo_buf_static), MPI_BYTE, 0, item->file->comm))
            D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't receive broadcasted dataset info")

        /* Decode oid */
        p = dinfo_buf_static;
        UINT64DECODE(p, dset->obj.oid.lo)
        UINT64DECODE(p, dset->obj.oid.hi)

        /* Decode serialized info lengths */
        UINT64DECODE(p, type_len)
        UINT64DECODE(p, space_len)
        UINT64DECODE(p, dcpl_len)
        tot_len = type_len + space_len + dcpl_len;

        /* Check for type_len set to 0 - indicates failure */
        if(type_len == 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "lead process failed to open dataset")

        /* Check if we need to perform another bcast */
        if(tot_len + (5 * sizeof(uint64_t)) > sizeof(dinfo_buf_static)) {
            /* Allocate a dynamic buffer if necessary */
            if(tot_len > sizeof(dinfo_buf_static)) {
                if(NULL == (dinfo_buf_dyn = (uint8_t *)DV_malloc(tot_len)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate space for dataset info")
                dinfo_buf = dinfo_buf_dyn;
            } /* end if */

            /* Receive dataset info */
            if(MPI_SUCCESS != MPI_Bcast((char *)dinfo_buf, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't receive broadcasted dataset info (second broadcast)")

            p = dinfo_buf;
        } /* end if */

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %s", H5_daos_err_to_string(ret))
    } /* end else */

    /* Decode datatype, dataspace, and DCPL */
    if((dset->type_id = H5Tdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    p += type_len;
    if((dset->space_id = H5Sdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize dataspace")
    if(H5Sselect_all(dset->space_id) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTDELETE, NULL, "can't change selection")
    p += space_len;
    if((dset->dcpl_id = H5Pdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize dataset creation property list")

    /* Finish setting up dataset struct */
    if((dset->dapl_id = H5Pcopy(dapl_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTCOPY, NULL, "failed to copy dapl");

    /* Set return value */
    ret_value = (void *)dset;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast dinfo_buf as '0' if necessary - this will trigger failures in
         * in other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(dinfo_buf_static, 0, sizeof(dinfo_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(dinfo_buf_static, sizeof(dinfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_DONE_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't broadcast empty dataset info")
        } /* end if */

        /* Close dataset */
        if(dset && H5_daos_dataset_close(dset, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close dataset")
    } /* end if */

    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    dinfo_buf_dyn = (uint8_t *)DV_free(dinfo_buf_dyn);

    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_sel_to_recx_iov
 *
 * Purpose:     Given a dataspace with a selection and the datatype
 *              (element) size, build a list of DAOS records (recxs)
 *              and/or scatter/gather list I/O vectors (sg_iovs). *recxs
 *              and *sg_iovs should, if requested, point to a (probably
 *              statically allocated) single element.  Does not release
 *              buffers on error.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_sel_to_recx_iov(hid_t space_id, size_t type_size, void *buf,
    daos_recx_t **recxs, daos_iov_t **sg_iovs, size_t *list_nused)
{
    size_t nseq;
    size_t nelem;
    hsize_t off[H5_DAOS_SEQ_LIST_LEN];
    size_t len[H5_DAOS_SEQ_LIST_LEN];
    size_t buf_len = 1;
    void *vp_ret;
    size_t szi;
    hid_t sel_iter = H5I_INVALID_HID;
    herr_t ret_value = SUCCEED;

    assert(recxs || sg_iovs);
    assert(!recxs || *recxs);
    assert(!sg_iovs || *sg_iovs);
    assert(list_nused);

    /* Initialize list_nused */
    *list_nused = 0;

    /* Initialize selection iterator */
    /*
     * DSINC - 1 for the element size doesn't seem right here, but using the datatype
     * size causes daos_obj_fetch to write outside of allocated memory buffers on a
     * dataset read. Need to investigate this further.
     */
    if((sel_iter = H5Ssel_iter_create(space_id, 1, H5S_SEL_ITER_SHARE_WITH_DATASPACE)) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTINIT, FAIL, "unable to create selection iterator")

    /* Generate sequences from the file space until finished */
    do {
        /* Get the sequences of bytes */
        if(H5Ssel_iter_get_seq_list(sel_iter, (size_t)H5_DAOS_SEQ_LIST_LEN, (size_t)-1, &nseq, &nelem, off, len) < 0)
            D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "sequence length generation failed")

        /* Make room for sequences in recxs */
        if((buf_len == 1) && (nseq > 1)) {
            if(recxs)
                if(NULL == (*recxs = (daos_recx_t *)DV_malloc(H5_DAOS_SEQ_LIST_LEN * sizeof(daos_recx_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate memory for records")
            if(sg_iovs)
                if(NULL == (*sg_iovs = (daos_iov_t *)DV_malloc(H5_DAOS_SEQ_LIST_LEN * sizeof(daos_iov_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate memory for sgl iovs")
            buf_len = H5_DAOS_SEQ_LIST_LEN;
        } /* end if */
        else if(*list_nused + nseq > buf_len) {
            if(recxs) {
                if(NULL == (vp_ret = DV_realloc(*recxs, 2 * buf_len * sizeof(daos_recx_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't reallocate memory for records")
                *recxs = (daos_recx_t *)vp_ret;
            } /* end if */
            if(sg_iovs) {
                if(NULL == (vp_ret = DV_realloc(*sg_iovs, 2 * buf_len * sizeof(daos_iov_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't reallocate memory for sgls")
                *sg_iovs = (daos_iov_t *)vp_ret;
            } /* end if */
            buf_len *= 2;
        } /* end if */
        assert(*list_nused + nseq <= buf_len);

        /* Copy offsets/lengths to recxs and sg_iovs */
        for(szi = 0; szi < nseq; szi++) {
            if(recxs) {
                (*recxs)[szi + *list_nused].rx_idx = (uint64_t)off[szi];
                (*recxs)[szi + *list_nused].rx_nr = (uint64_t)len[szi];
            } /* end if */
            if(sg_iovs)
                daos_iov_set(&(*sg_iovs)[szi + *list_nused],
                        (uint8_t *)buf + (off[szi] * type_size),
                        (daos_size_t)len[szi] * (daos_size_t)type_size);
        } /* end for */
        *list_nused += nseq;
    } while(nseq == H5_DAOS_SEQ_LIST_LEN);

done:
    /* Release selection iterator */
    if(sel_iter >= 0 && H5Ssel_iter_close(sel_iter) < 0)
        D_DONE_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to close selection iterator")

    D_FUNC_LEAVE
} /* end H5_daos_sel_to_recx_iov() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_scatter_cb
 *
 * Purpose:     Callback function for H5Dscatter.  Simply passes the
 *              entire buffer described by udata to H5Dscatter.
 *
 * Return:      SUCCEED (never fails)
 *
 * Programmer:  Neil Fortner
 *              March, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_scatter_cb(const void **src_buf, size_t *src_buf_bytes_used,
    void *_udata)
{
    H5_daos_scatter_cb_ud_t *udata = (H5_daos_scatter_cb_ud_t *)_udata;
    herr_t ret_value = SUCCEED;

    /* Set src_buf and src_buf_bytes_used to use the entire buffer */
    *src_buf = udata->buf;
    *src_buf_bytes_used = udata->len;

    /* DSINC - This function used to always return SUCCEED without needing an
     * herr_t. Might need an additional FUNC_LEAVE macro to do this, or modify
     * the current one to take in the ret_value.
     */
    D_FUNC_LEAVE
} /* end H5_daos_scatter_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_mem_vl_rd_cb
 *
 * Purpose:     H5Diterate callback for iterating over the memory space
 *              before reading vl data.  Allocates vl read buffers,
 *              up scatter gather lists (sgls), and reshapes iods if
 *              necessary to skip empty elements.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_mem_vl_rd_cb(void *_elem, hid_t H5VL_DAOS_UNUSED type_id,
    unsigned H5VL_DAOS_UNUSED ndim, const hsize_t H5VL_DAOS_UNUSED *point,
    void *_udata)
{
    H5_daos_vl_mem_ud_t *udata = (H5_daos_vl_mem_ud_t *)_udata;
    herr_t ret_value = SUCCEED;

    /* Set up constant sgl info */
    udata->sgls[udata->idx].sg_nr = 1;
    udata->sgls[udata->idx].sg_nr_out = 0;
    udata->sgls[udata->idx].sg_iovs = &udata->sg_iovs[udata->idx];

    /* Check for empty element */
    if(udata->iods[udata->idx].iod_size == 0) {
        /* Increment offset, slide down following elements */
        udata->offset++;

        /* Zero out read buffer */
        if(udata->is_vl_str)
            *(char **)_elem = NULL;
        else
            memset(_elem, 0, sizeof(hvl_t));
    } /* end if */
    else {
        assert(udata->idx >= udata->offset);

        /* Check for vlen string */
        if(udata->is_vl_str) {
            char *elem = NULL;

            /* Allocate buffer for this vl element */
            if(NULL == (elem = (char *)malloc((size_t)udata->iods[udata->idx].iod_size + 1)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate vl data buffer")
            *(char **)_elem = elem;

            /* Add null terminator */
            elem[udata->iods[udata->idx].iod_size] = '\0';

            /* Set buffer location in sgl */
            daos_iov_set(&udata->sg_iovs[udata->idx - udata->offset], elem, udata->iods[udata->idx].iod_size);
        } /* end if */
        else {
            /* Standard vlen, find hvl_t struct for this element */
            hvl_t *elem = (hvl_t *)_elem;

            assert(udata->base_type_size > 0);

            /* Allocate buffer for this vl element and set size */
            elem->len = (size_t)udata->iods[udata->idx].iod_size / udata->base_type_size;
            if(NULL == (elem->p = malloc((size_t)udata->iods[udata->idx].iod_size)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate vl data buffer")

            /* Set buffer location in sgl */
            daos_iov_set(&udata->sg_iovs[udata->idx - udata->offset], elem->p, udata->iods[udata->idx].iod_size);
        } /* end if */

        /* Slide down iod if necessary */
        if(udata->offset)
            udata->iods[udata->idx - udata->offset] = udata->iods[udata->idx];
    } /* end else */

    /* Advance idx */
    udata->idx++;

done:
    D_FUNC_LEAVE
} /* end H5_daos_dataset_mem_vl_rd_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_file_vl_cb
 *
 * Purpose:     H5Diterate callback for iterating over the file space
 *              before vl data I/O.  Sets up akeys and iods (except for
 *              iod record sizes).
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_file_vl_cb(void H5VL_DAOS_UNUSED *_elem,
    hid_t H5VL_DAOS_UNUSED type_id, unsigned ndim, const hsize_t *point,
    void *_udata)
{
    H5_daos_vl_file_ud_t *udata = (H5_daos_vl_file_ud_t *)_udata;
    size_t akey_len = ndim * sizeof(uint64_t);
    uint64_t coordu64;
    uint8_t *p;
    unsigned i;
    herr_t ret_value = SUCCEED;

    /* Create akey for this element */
    if(NULL == (udata->akeys[udata->idx] = (uint8_t *)DV_malloc(akey_len)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey")
    p = udata->akeys[udata->idx];
    for(i = 0; i < ndim; i++) {
        coordu64 = (uint64_t)point[i];
        UINT64ENCODE(p, coordu64)
    } /* end for */

    /* Set up iod, size was set in memory callback or initialized in main read
     * function.  Use "single" records of varying size. */
    daos_iov_set(&udata->iods[udata->idx].iod_name, (void *)udata->akeys[udata->idx], (daos_size_t)akey_len);
    daos_csum_set(&udata->iods[udata->idx].iod_kcsum, NULL, 0);
    udata->iods[udata->idx].iod_nr = 1u;
    udata->iods[udata->idx].iod_type = DAOS_IOD_SINGLE;

    /* Advance idx */
    udata->idx++;

done:
    D_FUNC_LEAVE
} /* end H5_daos_dataset_file_vl_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_read
 *
 * Purpose:     Reads raw data from a dataset into a buffer.
 *`
 * Return:      Success:        0
 *              Failure:        -1, dataset not read.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_dataset_read(void *_dset, hid_t mem_type_id, hid_t mem_space_id,
    hid_t file_space_id, hid_t dxpl_id, void *buf, void H5VL_DAOS_UNUSED **req)
{
    H5_daos_select_chunk_info_t *chunk_info = NULL; /* Array of info for each chunk selected in the file */
    H5_daos_chunk_io_func single_chunk_read_func;
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    H5T_class_t type_class;
    hssize_t num_elem = -1;
    uint64_t i;
    uint8_t dkey_buf[1 + (sizeof(uint64_t) * H5S_MAX_RANK)];
    hbool_t is_vl = FALSE;
    hbool_t close_spaces = FALSE;
    size_t chunk_info_len;
    hid_t real_file_space_id;
    hid_t real_mem_space_id;
    int ndims;
    herr_t ret_value = SUCCEED;

    if(!_dset)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "dataset object is NULL")
    if(H5I_DATASET != dset->obj.item.type)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "object is not a dataset")

    /* Get dataspace extent */
    if((ndims = H5Sget_simple_extent_ndims(dset->space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of dimensions")

    /* Get "real" space ids */
    if(file_space_id == H5S_ALL)
        real_file_space_id = dset->space_id;
    else
        real_file_space_id = file_space_id;
    if(mem_space_id == H5S_ALL)
        real_mem_space_id = real_file_space_id;
    else
        real_mem_space_id = mem_space_id;

    /* Get number of elements in selection */
    if((num_elem = H5Sget_select_npoints(real_file_space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")
    if(num_elem && !buf)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "read buffer is NULL but selection has >0 elements")

    /* Check for variable length */
    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        is_vl = TRUE;
    } /* end if */
    else if(type_class == H5T_STRING) {
        htri_t is_vl_str;

        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't check for variable length string")
        if(is_vl_str)
            is_vl = TRUE;
    } /* end if */

    /* Check for the dataset having a chunked storage layout. If it does not,
     * simply set up the dataset as a single "chunk".
     */
    switch(H5Pget_layout(dset->dcpl_id)) {
        case H5D_COMPACT:
        case H5D_CONTIGUOUS:
            if (NULL == (chunk_info = (H5_daos_select_chunk_info_t *) DV_malloc(sizeof(H5_daos_select_chunk_info_t))))
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, FAIL, "can't allocate single chunk info buffer")
            chunk_info_len = 1;

            /* Set up "single-chunk dataset", with the "chunk" starting at coordinate 0 */
            chunk_info->fspace_id = real_file_space_id;
            chunk_info->mspace_id = real_mem_space_id;
            memset(chunk_info->chunk_coords, 0, sizeof(chunk_info->chunk_coords));

            break;

        case H5D_CHUNKED:
            if(is_vl)
                D_GOTO_ERROR(H5E_DATASET, H5E_UNSUPPORTED, FAIL, "vlen types are currently unsupported with chunking")

            /* Get the coordinates of the currently selected chunks in the file, setting up memory and file dataspaces for them */
            if(H5_daos_get_selected_chunk_info(dset->dcpl_id, real_file_space_id, real_mem_space_id, &chunk_info, &chunk_info_len) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get selected chunk info")

            close_spaces = TRUE;

            break;

        case H5D_LAYOUT_ERROR:
        case H5D_NLAYOUTS:
        case H5D_VIRTUAL:
        default:
            D_GOTO_ERROR(H5E_DATASET, H5E_UNSUPPORTED, FAIL, "invalid, unknown or unsupported dataset storage layout type")
    } /* end switch */

    /* Setup the appropriate function for reading the selected chunks */
    if(is_vl) {
        single_chunk_read_func = H5_daos_dataset_io_vl;
    } /* end if */
    else {
        htri_t types_equal;

        /* Check if the types are equal */
        if((types_equal = H5Tequal(dset->type_id, mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTCOMPARE, FAIL, "can't check if types are equal")
        if(types_equal) {
            /* No type conversion necessary */
            single_chunk_read_func = H5_daos_dataset_io_types_equal;
        } /* end if */
        else {
            /* Type conversion necessary */
            single_chunk_read_func = H5_daos_dataset_io_types_unequal;
        } /* end else */
    } /* end else */

    /* Perform I/O on each chunk selected */
    for(i = 0; i < chunk_info_len; i++) {
        daos_key_t  dkey;
        uint64_t    j;
        uint8_t    *p = dkey_buf;

        /* Encode dkey (chunk coordinates).  Prefix with '\0' to avoid accidental
         * collisions with other d-keys in this object. */
        *p++ = (uint8_t)'\0';
        for(j = 0; j < (uint64_t)ndims; j++)
            UINT64ENCODE(p, chunk_info[i].chunk_coords[j])

        /* Set up dkey */
        daos_iov_set(&dkey, dkey_buf, (daos_size_t)(1 + ((size_t)ndims * sizeof(chunk_info[i].chunk_coords[0]))));

        /* Get number of elements in selection */
        if((num_elem = H5Sget_select_npoints(chunk_info[i].fspace_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")

        if(single_chunk_read_func(dset, dkey, num_elem, mem_type_id, chunk_info[i].mspace_id, chunk_info[i].fspace_id, dxpl_id, IO_READ, buf) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "dataset read failed")
    } /* end for */

done:
    if(chunk_info) {
        if(close_spaces) {
            for(i = 0; i < chunk_info_len; i++) {
                if((chunk_info[i].mspace_id >= 0) && (H5Sclose(chunk_info[i].mspace_id) < 0))
                    D_DONE_ERROR(H5E_DATASPACE, H5E_CANTCLOSEOBJ, FAIL, "can't close memory space");
                if((chunk_info[i].fspace_id >= 0) && (H5Sclose(chunk_info[i].fspace_id) < 0))
                    D_DONE_ERROR(H5E_DATASPACE, H5E_CANTCLOSEOBJ, FAIL, "can't close file space");
            } /* end for */
        } /* end if */

        DV_free(chunk_info);
    } /* end if */

    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_read() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_io_vl
 *
 * Purpose:     Internal helper routine to perform I/O on a dataset
 *              composed of a variable-length datatype.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset I/O not performed.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_io_vl(H5_daos_dset_t *dset, daos_key_t dkey, hssize_t num_elem,
    hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id, hid_t H5VL_DAOS_UNUSED dxpl_id,
    dset_io_type io_type, void *buf)
{
    H5_daos_vl_mem_ud_t mem_ud;
    H5_daos_vl_file_ud_t file_ud;
    daos_sg_list_t *sgls = NULL;
    daos_iov_t *sg_iovs = NULL;
    H5T_class_t type_class;
    daos_iod_t *iods = NULL;
    uint8_t **akeys = NULL;
    uint64_t i;
    htri_t is_vl_str = FALSE;
    size_t base_type_size = 0;
    hid_t base_type_id = FAIL;
    int ret;
    herr_t ret_value = SUCCEED;

    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        /* Calculate base type size */
        if((base_type_id = H5Tget_super(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype base type")
        if(0 == (base_type_size = H5Tget_size(base_type_id)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype base type size")
    } /* end if */
    else if(type_class == H5T_STRING) {
        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't check for variable length string")
    } /* end if */

    /* Allocate array of akey pointers */
    if(NULL == (akeys = (uint8_t **)DV_calloc((size_t)num_elem * sizeof(uint8_t *))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey array")

    /* Allocate array of iods */
    if(NULL == (iods = (daos_iod_t *)DV_calloc((size_t)num_elem * sizeof(daos_iod_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for I/O descriptor array")

    if(io_type == IO_READ) {
        /* Fill in size fields of iod as DAOS_REC_ANY so we can read the vl sizes */
        for(i = 0; i < (uint64_t)num_elem; i++)
            iods[i].iod_size = DAOS_REC_ANY;

        /* Iterate over file selection.  Note the bogus buffer and type_id, these
         * don't matter since the "elem" parameter of the callback is not used. */
        file_ud.akeys = akeys;
        file_ud.iods = iods;
        file_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, file_space_id, H5_daos_dataset_file_vl_cb, &file_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "file selection iteration failed")
        assert(file_ud.idx == (uint64_t)num_elem);

        /* Read vl sizes from dataset */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, (unsigned)num_elem, iods, NULL, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_READERROR, FAIL, "can't read vl data sizes from dataset: %s", H5_daos_err_to_string(ret))

        /* Allocate array of sg_iovs */
        if(NULL == (sg_iovs = (daos_iov_t *)DV_malloc((size_t)num_elem * sizeof(daos_iov_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list")

        /* Allocate array of sgls */
        if(NULL == (sgls = (daos_sg_list_t *)DV_malloc((size_t)num_elem * sizeof(daos_sg_list_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list array")

        /* Iterate over memory selection */
        mem_ud.iods = iods;
        mem_ud.sgls = sgls;
        mem_ud.sg_iovs = sg_iovs;
        mem_ud.is_vl_str = is_vl_str;
        mem_ud.base_type_size = base_type_size;
        mem_ud.offset = 0;
        mem_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, mem_space_id, H5_daos_dataset_mem_vl_rd_cb, &mem_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "memory selection iteration failed")
        assert(mem_ud.idx == (uint64_t)num_elem);

        /* Read data from dataset */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, (unsigned)((uint64_t)num_elem - mem_ud.offset), iods, sgls, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %s", H5_daos_err_to_string(ret))
    } /* end (io_type == IO_READ) */
    else {
        /* Allocate array of sg_iovs */
        if(NULL == (sg_iovs = (daos_iov_t *)DV_malloc((size_t)num_elem * sizeof(daos_iov_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list")

        /* Allocate array of sgls */
        if(NULL == (sgls = (daos_sg_list_t *)DV_malloc((size_t)num_elem * sizeof(daos_sg_list_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list array")

        /* Iterate over memory selection */
        mem_ud.iods = iods;
        mem_ud.sgls = sgls;
        mem_ud.sg_iovs = sg_iovs;
        mem_ud.is_vl_str = is_vl_str;
        mem_ud.base_type_size = base_type_size;
        mem_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, mem_space_id, H5_daos_dataset_mem_vl_wr_cb, &mem_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "memory selection iteration failed")
        assert(mem_ud.idx == (uint64_t)num_elem);

        /* Iterate over file selection.  Note the bogus buffer and type_id, these
         * don't matter since the "elem" parameter of the callback is not used. */
        file_ud.akeys = akeys;
        file_ud.iods = iods;
        file_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, file_space_id, H5_daos_dataset_file_vl_cb, &file_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "file selection iteration failed")
        assert(file_ud.idx == (uint64_t)num_elem);

        /* Write data to dataset */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, (unsigned)num_elem, iods, sgls, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_WRITEERROR, FAIL, "can't write data to dataset: %s", H5_daos_err_to_string(ret))
    } /* end (io_type == IO_WRITE) */

done:
    sgls = (daos_sg_list_t *)DV_free(sgls);
    sg_iovs = (daos_iov_t *)DV_free(sg_iovs);
    iods = (daos_iod_t *)DV_free(iods);

    if(akeys) {
        for(i = 0; i < (uint64_t)num_elem; i++)
            DV_free(akeys[i]);
        DV_free(akeys);
    } /* end if */

    if(base_type_id != FAIL)
        if(H5Idec_ref(base_type_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, FAIL, "can't close base type ID")

    D_FUNC_LEAVE
} /* end H5_daos_dataset_io_vl() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_io_types_equal
 *
 * Purpose:     Internal helper routine to perform I/O on a dataset
 *              composed of a non-variable-length datatype where the
 *              datatype specified for the memory buffer matches the
 *              dataset's datatype. In this case, datatype conversion is
 *              not necessary.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset I/O not performed.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_io_types_equal(H5_daos_dset_t *dset, daos_key_t dkey, hssize_t H5VL_DAOS_UNUSED num_elem,
    hid_t H5VL_DAOS_UNUSED mem_type_id, hid_t mem_space_id, hid_t file_space_id, hid_t H5VL_DAOS_UNUSED dxpl_id,
    dset_io_type io_type, void *buf)
{
    daos_sg_list_t sgl;
    daos_recx_t recx;
    daos_recx_t *recxs = &recx;
    daos_iov_t sg_iov;
    daos_iov_t *sg_iovs = &sg_iov;
    daos_iod_t iod;
    uint8_t akey = H5_DAOS_CHUNK_KEY;
    size_t tot_nseq;
    size_t file_type_size;
    int ret;
    herr_t ret_value = SUCCEED;

    /* Get datatype size */
    if((file_type_size = H5Tget_size(dset->type_id)) == 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype size for file datatype")

    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, (void *)&akey, (daos_size_t)(sizeof(akey)));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_size = file_type_size;
    iod.iod_type = DAOS_IOD_ARRAY;

    /* Check for a memory space of H5S_ALL, use file space in this case */
    if(mem_space_id == H5S_ALL) {
        /* Calculate both recxs and sg_iovs at the same time from file space */
        if(H5_daos_sel_to_recx_iov(file_space_id, file_type_size, buf, &recxs, &sg_iovs, &tot_nseq) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
        iod.iod_nr = (unsigned)tot_nseq;
        sgl.sg_nr = (uint32_t)tot_nseq;
        sgl.sg_nr_out = 0;
    } /* end if */
    else {
        /* Calculate recxs from file space */
        if(H5_daos_sel_to_recx_iov(file_space_id, file_type_size, buf, &recxs, NULL, &tot_nseq) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
        iod.iod_nr = (unsigned)tot_nseq;

        /* Calculate sg_iovs from mem space */
        if(H5_daos_sel_to_recx_iov(mem_space_id, file_type_size, buf, NULL, &sg_iovs, &tot_nseq) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
        sgl.sg_nr = (uint32_t)tot_nseq;
        sgl.sg_nr_out = 0;
    } /* end else */

    /* Point iod and sgl to lists generated above */
    iod.iod_recxs = recxs;
    sgl.sg_iovs = sg_iovs;

    /* No selection in the file */
    if(iod.iod_nr == 0)
        D_GOTO_DONE(SUCCEED);

    if(io_type == IO_READ) {
        /* Read data from dataset */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %s", H5_daos_err_to_string(ret))
    } /* end (io_type == IO_READ) */
    else {
        /* Write data to dataset */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_WRITEERROR, FAIL, "can't write data to dataset: %s", H5_daos_err_to_string(ret))
    } /* end (io_type == IO_WRITE) */

done:
    /* Free memory */
    if(recxs != &recx)
        DV_free(recxs);
    if(sg_iovs != &sg_iov)
        DV_free(sg_iovs);

    D_FUNC_LEAVE
} /* end H5_daos_dataset_io_types_equal() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_io_types_unequal
 *
 * Purpose:     Internal helper routine to perform I/O on a dataset
 *              composed of a non-variable-length datatype where the
 *              datatype specified for the memory buffer doesn't match the
 *              dataset's datatype. In this case, datatype conversion must
 *              be performed.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset I/O not performed.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_io_types_unequal(H5_daos_dset_t *dset, daos_key_t dkey, hssize_t num_elem,
    hid_t mem_type_id, hid_t mem_space_id, hid_t file_space_id, hid_t dxpl_id,
    dset_io_type io_type, void *buf)
{
    H5_daos_tconv_reuse_t reuse = H5_DAOS_TCONV_REUSE_NONE;
    daos_sg_list_t sgl;
    daos_recx_t recx;
    daos_recx_t *recxs = &recx;
    daos_iov_t sg_iov;
    daos_iov_t *sg_iovs = &sg_iov;
    daos_iod_t iod;
    uint8_t akey = H5_DAOS_CHUNK_KEY;
    hbool_t contig = FALSE;
    hbool_t fill_bkg = FALSE;
    size_t tot_nseq;
    size_t mem_type_size;
    size_t file_type_size;
    hid_t sel_iter = H5I_INVALID_HID;
    void *tconv_buf = NULL;
    void *bkg_buf = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    if(io_type == IO_READ) {
        size_t nseq_tmp;
        size_t nelem_tmp;
        hsize_t sel_off;
        size_t sel_len;

        /* Check for contiguous memory buffer */

        if((mem_type_size = H5Tget_size(mem_type_id)) == 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get memory datatype size")
        if((file_type_size = H5Tget_size(dset->type_id)) == 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get dataset's file datatype size")

        /* Initialize selection iterator */
        if((sel_iter = H5Ssel_iter_create(mem_space_id, file_type_size, H5S_SEL_ITER_SHARE_WITH_DATASPACE)) < 0)
            D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTINIT, FAIL, "unable to create selection iterator")

        /* Get the sequence list - only check the first sequence because we only
         * care if it is contiguous and if so where the contiguous selection
         * begins */
        if(H5Ssel_iter_get_seq_list(sel_iter, (size_t)1, (size_t)-1, &nseq_tmp, &nelem_tmp, &sel_off, &sel_len) < 0)
            D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "sequence length generation failed")
        contig = (sel_len == (size_t)num_elem);

        /* Reuse buffer as appropriate */
        if(contig) {
            sel_off *= (hsize_t)mem_type_size;
            if(reuse == H5_DAOS_TCONV_REUSE_TCONV)
                tconv_buf = (char *)buf + (size_t)sel_off;
            else if(reuse == H5_DAOS_TCONV_REUSE_BKG)
                bkg_buf = (char *)buf + (size_t)sel_off;
        } /* end if */
    } /* end (io_type == IO_READ) */

    /* Initialize type conversion */
    if(H5_daos_tconv_init(
            /* If reading, source type ID/source type size correspond to the values
             * from the dataset's file datatype. Otherwise, the source type ID/source
             * type size correspond to the values from the memory type ID. */
            (io_type == IO_READ) ? dset->type_id : mem_type_id,
            (io_type == IO_READ) ? &file_type_size : &mem_type_size,
            (io_type == IO_READ) ? mem_type_id : dset->type_id,
            (io_type == IO_READ) ? &mem_type_size : &file_type_size,
            (size_t)num_elem,
            &tconv_buf,
            &bkg_buf,
            (contig && (io_type == IO_READ)) ? &reuse : NULL,
            &fill_bkg) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't initialize type conversion")

    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, (void *)&akey, (daos_size_t)(sizeof(akey)));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_size = file_type_size;
    iod.iod_type = DAOS_IOD_ARRAY;

    /* Build recxs and sg_iovs */

    /* Calculate recxs from file space */
    if(H5_daos_sel_to_recx_iov(file_space_id, file_type_size, buf, &recxs, NULL, &tot_nseq) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
    iod.iod_nr = (unsigned)tot_nseq;
    iod.iod_recxs = recxs;

    /* No selection in the file */
    if(iod.iod_nr == 0)
        D_GOTO_DONE(SUCCEED);

    /* Set up constant sgl info */
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &sg_iov;

    if(io_type == IO_READ) {
        /* Set sg_iov to point to tconv_buf */
        daos_iov_set(&sg_iov, tconv_buf, (daos_size_t)num_elem * (daos_size_t)file_type_size);

        /* Read data to tconv_buf */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %s", H5_daos_err_to_string(ret))

        /* Gather data to background buffer if necessary */
        if(fill_bkg && (reuse != H5_DAOS_TCONV_REUSE_BKG))
            if(H5Dgather(mem_space_id, buf, mem_type_id, (size_t)num_elem * mem_type_size, bkg_buf, NULL, NULL) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't gather data to background buffer")

        /* Perform type conversion */
        if(H5Tconvert(dset->type_id, mem_type_id, (size_t)num_elem, tconv_buf, bkg_buf, dxpl_id) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTCONVERT, FAIL, "can't perform type conversion")

        /* Scatter data to memory buffer if necessary */
        if(reuse != H5_DAOS_TCONV_REUSE_TCONV) {
            H5_daos_scatter_cb_ud_t scatter_cb_ud;

            scatter_cb_ud.buf = tconv_buf;
            scatter_cb_ud.len = (size_t)num_elem * mem_type_size;
            if(H5Dscatter(H5_daos_scatter_cb, &scatter_cb_ud, mem_type_id, mem_space_id, buf) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't scatter data to read buffer")
        } /* end if */
    } /* end (io_type == IO_READ) */
    else {
        /* Check if we need to fill background buffer */
        if(fill_bkg) {
            assert(bkg_buf);

            /* Set sg_iov to point to background buffer */
            daos_iov_set(&sg_iov, bkg_buf, (daos_size_t)num_elem * (daos_size_t)file_type_size);

            /* Read data from dataset to background buffer */
            if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
                D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %s", H5_daos_err_to_string(ret))

            /* Reset iod_size, if the dataset was not allocated then it could
             * have been overwritten by daos_obj_fetch */
            iod.iod_size = file_type_size;
        } /* end if */

        /* Gather data to conversion buffer */
        if(H5Dgather(mem_space_id, buf, mem_type_id, (size_t)num_elem * mem_type_size, tconv_buf, NULL, NULL) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't gather data to conversion buffer")

        /* Perform type conversion */
        if(H5Tconvert(mem_type_id, dset->type_id, (size_t)num_elem, tconv_buf, bkg_buf, dxpl_id) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTCONVERT, FAIL, "can't perform type conversion")

        /* Set sg_iovs to write from tconv_buf */
        daos_iov_set(&sg_iov, tconv_buf, (daos_size_t)num_elem * (daos_size_t)file_type_size);

        /* Write data to dataset */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_WRITEERROR, FAIL, "can't write data to dataset: %s", H5_daos_err_to_string(ret))
    } /* end (io_type == IO_WRITE) */

done:
    if(recxs != &recx)
        DV_free(recxs);
    if(sg_iovs != &sg_iov)
        DV_free(sg_iovs);

    if((io_type == IO_WRITE) || (io_type == IO_READ && reuse != H5_DAOS_TCONV_REUSE_TCONV))
        tconv_buf = DV_free(tconv_buf);
    if((io_type == IO_WRITE) || (io_type == IO_READ && reuse != H5_DAOS_TCONV_REUSE_BKG))
        bkg_buf = DV_free(bkg_buf);

    /* Release selection iterator */
    if(sel_iter >= 0 && H5Ssel_iter_close(sel_iter) < 0)
        D_DONE_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to close selection iterator")

    D_FUNC_LEAVE
} /* end H5_daos_dataset_io_types_unequal() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_mem_vl_wr_cb
 *
 * Purpose:     H5Diterate callback for iterating over the memory space
 *              before writing vl data.  Sets up scatter gather lists
 *              (sgls) and sets the record sizes in iods.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_mem_vl_wr_cb(void *_elem, hid_t H5VL_DAOS_UNUSED type_id,
    unsigned H5VL_DAOS_UNUSED ndim, const hsize_t H5VL_DAOS_UNUSED *point,
    void *_udata)
{
    H5_daos_vl_mem_ud_t *udata = (H5_daos_vl_mem_ud_t *)_udata;
    herr_t ret_value = SUCCEED;

    /* Set up constant sgl info */
    udata->sgls[udata->idx].sg_nr = 1;
    udata->sgls[udata->idx].sg_nr_out = 0;
    udata->sgls[udata->idx].sg_iovs = &udata->sg_iovs[udata->idx];

    /* Check for vlen string */
    if(udata->is_vl_str) {
        /* Find string for this element */
        char *elem = *(char **)_elem;

        /* Set string length in iod and buffer location in sgl.  If we are
         * writing an empty string ("\0"), increase the size by one to
         * differentiate it from NULL strings.  Note that this will cause the
         * read buffer to be one byte longer than it needs to be in this case.
         * This should not cause any ill effects. */
        if(elem) {
            udata->iods[udata->idx].iod_size = (daos_size_t)strlen(elem);
            if(udata->iods[udata->idx].iod_size == 0)
                udata->iods[udata->idx].iod_size = 1;
            daos_iov_set(&udata->sg_iovs[udata->idx], (void *)elem, udata->iods[udata->idx].iod_size);
        } /* end if */
        else {
            udata->iods[udata->idx].iod_size = 0;
            daos_iov_set(&udata->sg_iovs[udata->idx], NULL, 0);
        } /* end else */
    } /* end if */
    else {
        /* Standard vlen, find hvl_t struct for this element */
        hvl_t *elem = (hvl_t *)_elem;

        assert(udata->base_type_size > 0);

        /* Set buffer length in iod and buffer location in sgl */
        if(elem->len > 0) {
            udata->iods[udata->idx].iod_size = (daos_size_t)(elem->len * udata->base_type_size);
            daos_iov_set(&udata->sg_iovs[udata->idx], (void *)elem->p, udata->iods[udata->idx].iod_size);
        } /* end if */
        else {
            udata->iods[udata->idx].iod_size = 0;
            daos_iov_set(&udata->sg_iovs[udata->idx], NULL, 0);
        } /* end else */
    } /* end else */

    /* Advance idx */
    udata->idx++;

    /* DSINC - This function used to always return SUCCEED without needing an
     * herr_t. Might need an additional FUNC_LEAVE macro to do this, or modify
     * the current one to take in the ret_value.
     */
    D_FUNC_LEAVE
} /* end H5_daos_dataset_mem_vl_wr_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_write
 *
 * Purpose:     Writes raw data from a buffer into a dataset.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_dataset_write(void *_dset, hid_t mem_type_id, hid_t mem_space_id,
    hid_t file_space_id, hid_t H5VL_DAOS_UNUSED dxpl_id,
    const void *buf, void H5VL_DAOS_UNUSED **req)
{
    H5_daos_select_chunk_info_t *chunk_info = NULL; /* Array of info for each chunk selected in the file */
    H5_daos_chunk_io_func single_chunk_write_func;
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    H5T_class_t type_class;
    hssize_t num_elem = -1;
    uint64_t i;
    uint8_t dkey_buf[1 + (sizeof(uint64_t) * H5S_MAX_RANK)];
    hbool_t is_vl = FALSE;
    hbool_t close_spaces = FALSE;
    size_t chunk_info_len;
    hid_t real_file_space_id;
    hid_t real_mem_space_id;
    int ndims;
    herr_t ret_value = SUCCEED;

    if(!_dset)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "dataset object is NULL")
    if(H5I_DATASET != dset->obj.item.type)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "object is not a dataset")

    /* Check for write access */
    if(!(dset->obj.item.file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file")

    /* Get dataspace extent */
    if((ndims = H5Sget_simple_extent_ndims(dset->space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of dimensions")

    /* Get "real" space ids */
    if(file_space_id == H5S_ALL)
        real_file_space_id = dset->space_id;
    else
        real_file_space_id = file_space_id;
    if(mem_space_id == H5S_ALL)
        real_mem_space_id = real_file_space_id;
    else
        real_mem_space_id = mem_space_id;

    /* Get number of elements in selection */
    if((num_elem = H5Sget_select_npoints(real_file_space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")
    if(num_elem && !buf)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "write buffer is NULL but selection has >0 elements")

    /* Check for variable length */
    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        is_vl = TRUE;
    } /* end if */
    else if(type_class == H5T_STRING) {
        htri_t is_vl_str = FALSE;

        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't check for variable length string")
        if(is_vl_str)
            is_vl = TRUE;
    } /* end if */

    /* Check for the dataset having a chunked storage layout. If it does not,
     * simply set up the dataset as a single "chunk".
     */
    switch(H5Pget_layout(dset->dcpl_id)) {
        case H5D_COMPACT:
        case H5D_CONTIGUOUS:
            if (NULL == (chunk_info = (H5_daos_select_chunk_info_t *) DV_malloc(sizeof(H5_daos_select_chunk_info_t))))
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, FAIL, "can't allocate single chunk info buffer")
            chunk_info_len = 1;

            /* Set up "single-chunk dataset", with the "chunk" starting at coordinate 0 */
            chunk_info->fspace_id = real_file_space_id;
            chunk_info->mspace_id = real_mem_space_id;
            memset(chunk_info->chunk_coords, 0, sizeof(chunk_info->chunk_coords));

            break;

        case H5D_CHUNKED:
            if(is_vl)
                D_GOTO_ERROR(H5E_DATASET, H5E_UNSUPPORTED, FAIL, "vlen types are currently unsupported with chunking")

            /* Get the coordinates of the currently selected chunks in the file, setting up memory and file dataspaces for them */
            if(H5_daos_get_selected_chunk_info(dset->dcpl_id, real_file_space_id, real_mem_space_id, &chunk_info, &chunk_info_len) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get selected chunk info")

            close_spaces = TRUE;

            break;

        case H5D_LAYOUT_ERROR:
        case H5D_NLAYOUTS:
        case H5D_VIRTUAL:
        default:
            D_GOTO_ERROR(H5E_DATASET, H5E_UNSUPPORTED, FAIL, "invalid, unknown or unsupported dataset storage layout type")
    } /* end switch */

    /* Setup the appropriate function for writing the selected chunks */
    if(is_vl) {
        single_chunk_write_func = H5_daos_dataset_io_vl;
    } /* end if */
    else {
        htri_t types_equal;

        /* Check if the types are equal */
        if((types_equal = H5Tequal(dset->type_id, mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTCOMPARE, FAIL, "can't check if types are equal")
        if(types_equal) {
            /* No type conversion necessary */
            single_chunk_write_func = H5_daos_dataset_io_types_equal;
        } /* end if */
        else {
            /* Type conversion necessary */
            single_chunk_write_func = H5_daos_dataset_io_types_unequal;
        } /* end else */
    } /* end else */

    /* Perform I/O on each chunk selected */
    for(i = 0; i < chunk_info_len; i++) {
        daos_key_t  dkey;
        uint64_t    j;
        uint8_t    *p = dkey_buf;

        /* Encode dkey (chunk coordinates).  Prefix with '\0' to avoid accidental
         * collisions with other d-keys in this object. */
        *p++ = (uint8_t)'\0';
        for(j = 0; j < (uint64_t)ndims; j++)
            UINT64ENCODE(p, chunk_info[i].chunk_coords[j])

        /* Set up dkey */
        daos_iov_set(&dkey, dkey_buf, (daos_size_t)(1 + ((size_t)ndims * sizeof(chunk_info[i].chunk_coords[0]))));

        /* Get number of elements in selection */
        if((num_elem = H5Sget_select_npoints(chunk_info[i].fspace_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")

        if(single_chunk_write_func(dset, dkey, num_elem, mem_type_id, chunk_info[i].mspace_id, chunk_info[i].fspace_id, dxpl_id, IO_WRITE, buf) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_WRITEERROR, FAIL, "dataset write failed")
    } /* end for */

done:
    if(chunk_info) {
        if(close_spaces) {
            for(i = 0; i < chunk_info_len; i++) {
                if((chunk_info[i].mspace_id >= 0) && (H5Sclose(chunk_info[i].mspace_id) < 0))
                    D_DONE_ERROR(H5E_DATASPACE, H5E_CANTCLOSEOBJ, FAIL, "can't close memory space");
                if((chunk_info[i].fspace_id >= 0) && (H5Sclose(chunk_info[i].fspace_id) < 0))
                    D_DONE_ERROR(H5E_DATASPACE, H5E_CANTCLOSEOBJ, FAIL, "can't close file space");
            } /* end for */
        } /* end if */

        DV_free(chunk_info);
    } /* end if */

    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_write() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_get
 *
 * Purpose:     Gets certain information about a dataset
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_dataset_get(void *_dset, H5VL_dataset_get_t get_type,
    hid_t H5VL_DAOS_UNUSED dxpl_id, void H5VL_DAOS_UNUSED **req, va_list arguments)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    herr_t       ret_value = SUCCEED;    /* Return value */

    if(!_dset)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "VOL object is NULL")

    switch (get_type) {
        case H5VL_DATASET_GET_DCPL:
            {
                hid_t *plist_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's creation property list */
                if((*plist_id = H5Pcopy(dset->dcpl_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dataset creation property list")

                break;
            } /* end block */
        case H5VL_DATASET_GET_DAPL:
            {
                hid_t *plist_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's access property list */
                if((*plist_id = H5Pcopy(dset->dapl_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dataset access property list")

                break;
            } /* end block */
        case H5VL_DATASET_GET_SPACE:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's dataspace */
                if((*ret_id = H5Scopy(dset->space_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dataspace ID of dataset");
                break;
            } /* end block */
        case H5VL_DATASET_GET_SPACE_STATUS:
            {
                H5D_space_status_t *allocation = va_arg(arguments, H5D_space_status_t *);

                /* Retrieve the dataset's space status */
                *allocation = H5D_SPACE_STATUS_NOT_ALLOCATED;
                break;
            } /* end block */
        case H5VL_DATASET_GET_TYPE:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's datatype */
                if((*ret_id = H5Tcopy(dset->type_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype ID of dataset")
                break;
            } /* end block */
        case H5VL_DATASET_GET_STORAGE_SIZE:
        case H5VL_DATASET_GET_OFFSET:
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "can't get this type of information from dataset")
    } /* end switch */

done:
    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_get() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_specific
 *
 * Purpose:     Performs a dataset "specific" operation
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Jordan Henderson
 *              January, 2019
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_dataset_specific(void *_item, H5VL_dataset_specific_t specific_type,
    hid_t dxpl_id, void **req, va_list arguments)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_item;
    herr_t          ret_value = SUCCEED;

    if(!_item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "VOL object is NULL")
    if(H5I_DATASET != dset->obj.item.type)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "object is not a dataset")

    switch (specific_type) {
        case H5VL_DATASET_SET_EXTENT:
            {
                H5D_layout_t storage_layout;
                const hsize_t *size = va_arg(arguments, const hsize_t *);

                if(!size)
                    D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "size parameter is NULL")

                if (H5D_LAYOUT_ERROR == (storage_layout = H5Pget_layout(dset->dcpl_id)))
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "failed to retrieve dataset storage layout")

                if (H5D_CHUNKED != storage_layout)
                    D_GOTO_ERROR(H5E_DATASET, H5E_BADVALUE, FAIL, "dataset storage layout is not chunked")

                /* Call main routine */
                if(H5_daos_dataset_set_extent(dset, size, dxpl_id, req) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "failed to set dataset extent")

                break;
            } /* end block */

        case H5VL_DATASET_FLUSH:
        case H5VL_DATASET_REFRESH:
            /* No-ops */
            break;

        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "invalid or unsupported dataset specific operation")
    }  /* end switch */

done:
    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_specific() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_set_extent
 *
 * Purpose:     Changes the extent of a dataset
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              July, 2019
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_set_extent(H5_daos_dset_t *dset, const hsize_t *size,
    hid_t H5VL_DAOS_UNUSED dxpl_id, void H5VL_DAOS_UNUSED **req)
{
    hsize_t maxdims[H5S_MAX_RANK];
    int ndims;
    void *space_buf = NULL;
    hbool_t collective;
    int i;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(dset);
    assert(size);

    /* Check for write access */
    if(!(dset->obj.item.file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file")

    /*
     * Like HDF5, all metadata writes are collective by default. Once independent
     * metadata writes are implemented, we will need to check for this property.
     */
    collective = TRUE;

    /* Get dataspace rank */
    if((ndims = H5Sget_simple_extent_ndims(dset->space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get current dataspace rank")

    /* Get dataspace max dims */
    if(H5Sget_simple_extent_dims(dset->space_id, NULL, maxdims) <0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get current dataspace maximum dimensions")

    /* Make sure max dims aren't exceeded */
    for(i = 0; i < ndims; i++)
        if((maxdims[i] != H5S_UNLIMITED) && (size[i] > maxdims[i]))
            D_GOTO_ERROR(H5E_ARGS, H5E_BADRANGE, FAIL, "requested dataset dimensions exceed maximum dimensions")

    /* Change dataspace extent */
    if(H5Sset_extent_simple(dset->space_id, ndims, size, maxdims) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTSET, FAIL, "can't set dataspace dimensions")

    /* Write new dataspace to dataset in file if this process should */
    if(!collective || (dset->obj.item.file->my_rank == 0)) {
        daos_key_t dkey;
        daos_iod_t iod;
        daos_sg_list_t sgl;
        daos_iov_t sg_iov;
        size_t space_size = 0;

        /* Encode dataspace */
        if(H5Sencode2(dset->space_id, NULL, &space_size, dset->obj.item.file->fapl_id) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "can't determine serialized length of dataspace")
        if(NULL == (space_buf = DV_malloc(space_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for serialized dataspace")
        if(H5Sencode2(dset->space_id, space_buf, &space_size, dset->obj.item.file->fapl_id) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, FAIL, "can't serialize dataspace")

        /* Set up operation to write dataspace to dataset */
        /* Set up dkey */
        daos_iov_set(&dkey, (void *)H5_daos_int_md_key_g, H5_daos_int_md_key_size_g);

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)H5_daos_space_key_g, H5_daos_space_key_size_g);
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_size = (uint64_t)space_size;
        iod.iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov, space_buf, (daos_size_t)space_size);
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        /* Write updated dataspace to dataset */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't write metadata to dataset: %s", H5_daos_err_to_string(ret))
    } /* end if */

done:
    /* Free memory */
    space_buf = DV_free(space_buf);

    D_FUNC_LEAVE
} /* end H5_daos_dataset_set_extent() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_close
 *
 * Purpose:     Closes a DAOS HDF5 dataset.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_dataset_close(void *_dset, hid_t H5VL_DAOS_UNUSED dxpl_id,
    void H5VL_DAOS_UNUSED **req)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    int ret;
    herr_t ret_value = SUCCEED;

    if(!_dset)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "dataset object is NULL")

    if(--dset->obj.item.rc == 0) {
        /* Free dataset data structures */
        if(dset->obj.item.open_req)
            H5_daos_req_free_int(dset->obj.item.open_req);
        if(!daos_handle_is_inval(dset->obj.obj_oh))
            if(0 != (ret = daos_obj_close(dset->obj.obj_oh, NULL /*event*/)))
                D_DONE_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "can't close dataset DAOS object: %s", H5_daos_err_to_string(ret))
        if(dset->type_id != FAIL && H5Idec_ref(dset->type_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close dataset's datatype")
        if(dset->space_id != FAIL && H5Idec_ref(dset->space_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close dataset's dataspace")
        if(dset->dcpl_id != FAIL && H5Idec_ref(dset->dcpl_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close dcpl")
        if(dset->dapl_id != FAIL && H5Idec_ref(dset->dapl_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close dapl")
        dset = H5FL_FREE(H5_daos_dset_t, dset);
    } /* end if */

done:
    D_FUNC_LEAVE_API
} /* end H5_daos_dataset_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_get_selected_chunk_info
 *
 * Purpose:     Calculates the starting coordinates for the chunks selected
 *              in the file space given by file_space_id and sets up
 *              individual memory and file spaces for each chunk. The chunk
 *              coordinates and dataspaces are returned through the
 *              chunk_info struct pointer.
 *
 *              XXX: Note that performance could be increased by
 *                   calculating all of the chunks in the entire dataset
 *                   and then caching them in the dataset object for
 *                   re-use in subsequent reads/writes
 *
 * Return:      Success: 0
 *              Failure: -1
 *
 * Programmer:  Neil Fortner
 *              May, 2018
 *              Based on H5VL_daosm_get_selected_chunk_info by Jordan
 *              Henderson, May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_get_selected_chunk_info(hid_t dcpl_id,
    hid_t file_space_id, hid_t mem_space_id,
    H5_daos_select_chunk_info_t **chunk_info, size_t *chunk_info_len)
{
    H5_daos_select_chunk_info_t *_chunk_info = NULL;
    hssize_t  num_sel_points;
    hssize_t  chunk_file_space_adjust[H5O_LAYOUT_NDIMS];
    hsize_t   file_space_dims[H5S_MAX_RANK];
    hsize_t   chunk_dims[H5S_MAX_RANK], partial_chunk_dims[H5S_MAX_RANK] = {0};
    hsize_t   file_sel_start[H5S_MAX_RANK], file_sel_end[H5S_MAX_RANK];
    hsize_t   mem_sel_start[H5S_MAX_RANK], mem_sel_end[H5S_MAX_RANK];
    hsize_t   start_coords[H5O_LAYOUT_NDIMS], end_coords[H5O_LAYOUT_NDIMS];
    hsize_t   selection_start_coords[H5O_LAYOUT_NDIMS] = {0};
    hsize_t   num_sel_points_cast;
    hbool_t   is_all_file_space = FALSE;
    htri_t    space_same_shape = FALSE;
    size_t    info_buf_alloced;
    size_t    i = 0, j;
    hid_t     tmp_chunk_fspace_id = H5I_INVALID_HID;
    hid_t     tmp_chunk_mspace_id = H5I_INVALID_HID;
    int       fspace_ndims, mspace_ndims;
    int       increment_dim;
    herr_t    ret_value = SUCCEED;

    assert(chunk_info);
    assert(chunk_info_len);

    if ((num_sel_points = H5Sget_select_npoints(file_space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_BADVALUE, FAIL, "can't get number of points selected in file dataspace");
    /* H5_CHECKED_ASSIGN(num_sel_points_cast, hsize_t, num_sel_points, hssize_t); */
    num_sel_points_cast = (hsize_t) num_sel_points;

    if (num_sel_points == 0)
        D_GOTO_DONE(SUCCEED);

    /* Get the chunking information */
    if (H5Pget_chunk(dcpl_id, H5S_MAX_RANK, chunk_dims) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get chunking information");

    if ((fspace_ndims = H5Sget_simple_extent_ndims(file_space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "can't get file space dimensionality");
    if ((mspace_ndims = H5Sget_simple_extent_ndims(mem_space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "can't get memory space dimensionality");
    assert(mspace_ndims == fspace_ndims);

    if (H5Sget_simple_extent_dims(file_space_id, file_space_dims, NULL) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "can't get file dataspace dimensions")

    /* Get the bounding box for the current selection in the file and memory spaces */
    if (H5Sget_select_bounds(file_space_id, file_sel_start, file_sel_end) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "can't get bounding box for file selection");
    if (H5Sget_select_bounds(mem_space_id, mem_sel_start, mem_sel_end) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "can't get bounding box for memory selection");

    /*
     * DSINC - temporary workaround for ALL selections.
     */
    {
        hsize_t file_points;

        for (i = 0, file_points = 1; i < (hsize_t) fspace_ndims; i++) {
            hsize_t dim_sel_points = file_sel_end[i] - file_sel_start[i] + 1;
            file_points *= (dim_sel_points > 0) ? dim_sel_points : 1;
        }
        if (file_points == (hsize_t) num_sel_points)
            is_all_file_space = TRUE;
    }

    /* Calculate the adjustment for the memory selection from the file selection */
    for (i = 0; i < (size_t) fspace_ndims; i++) {
        /* H5_CHECK_OVERFLOW(file_sel_start[i], hsize_t, hssize_t); */
        /* H5_CHECK_OVERFLOW(mem_sel_start[i], hsize_t, hssize_t); */
        chunk_file_space_adjust[i] = (hssize_t) file_sel_start[i] - (hssize_t) mem_sel_start[i];
    } /* end for */

    if (NULL == (_chunk_info = (H5_daos_select_chunk_info_t *) DV_malloc(H5_DAOS_DEFAULT_NUM_SEL_CHUNKS * sizeof(*_chunk_info))))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, FAIL, "can't allocate space for selected chunk info buffer");
    info_buf_alloced = H5_DAOS_DEFAULT_NUM_SEL_CHUNKS * sizeof(*_chunk_info);

    /* Calculate the coordinates for the initial chunk */
    for (i = 0; i < (size_t) fspace_ndims; i++) {
        start_coords[i] = selection_start_coords[i] = (file_sel_start[i] / chunk_dims[i]) * chunk_dims[i];
        end_coords[i] = (start_coords[i] + chunk_dims[i]) - 1;
    } /* end for */

    if (FAIL == (space_same_shape = H5Sselect_shape_same(file_space_id, mem_space_id)))
        D_GOTO_ERROR(H5E_DATASPACE, H5E_BADVALUE, FAIL, "can't determine if file and memory dataspaces are the same shape");

    /* Iterate through each "chunk" in the dataset */
    for (i = 0; num_sel_points_cast;) {
        htri_t intersect = FALSE;

        /* Check for intersection of file selection and "chunk". If there is
         * an intersection, set up a valid memory and file space for the chunk. */
        if (is_all_file_space)
            intersect = TRUE;
        else
            if ((intersect = H5Shyper_intersect_block(file_space_id, start_coords, end_coords)) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_BADVALUE, FAIL, "cannot determine chunk's intersection with the file dataspace");
        if (TRUE == intersect) {
            hssize_t chunk_mem_space_adjust[H5O_LAYOUT_NDIMS];
            hssize_t chunk_sel_npoints;
            hbool_t  is_partial_edge_chunk = FALSE;

            /* Re-allocate selected chunk info buffer if necessary */
            while (i > (info_buf_alloced / sizeof(*_chunk_info)) - 1) {
                if (NULL == (_chunk_info = (H5_daos_select_chunk_info_t *) DV_realloc(_chunk_info, 2 * info_buf_alloced)))
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, FAIL, "can't reallocate space for selected chunk info buffer");
                info_buf_alloced *= 2;
            } /* end while */

            /*
             * Set up the file Dataspace for this chunk.
             */

            /* Create temporary chunk for selection operations */
            if ((tmp_chunk_fspace_id = H5Scopy(file_space_id)) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTCOPY, FAIL, "unable to copy file space");

            /* Make certain selections are stored in span tree form (not "optimized hyperslab" or "all") */
            /* TODO check whether this is still necessary after hyperslab update merge */
#if 0
            if (H5Shyper_convert(tmp_chunk_fspace_id) < 0) {
                H5Sclose(tmp_chunk_fspace_id);
                HGOTO_ERROR(H5E_DATASPACE, H5E_CANTINIT, FAIL, "unable to convert selection to span trees");
            } /* end if */
#endif

            /* Determine if the current chunk is a partial edge chunk */
            if ((is_partial_edge_chunk = H5_daos_is_partial_edge_chunk((unsigned) fspace_ndims,
                    file_space_dims, chunk_dims, start_coords))) {
                /* If this is a partial edge chunk, setup the partial edge chunk dimensions.
                 * These will be used to adjust the selection within the edge chunk so that
                 * it falls within the dataset's dataspace boundaries.
                 */
                for (j = 0; j < (size_t) fspace_ndims; j++) {
                    if (start_coords[j] + chunk_dims[j] > file_space_dims[j]) {
                        size_t n_elems_beyond_edge = start_coords[j] + chunk_dims[j] - file_space_dims[j];

                        partial_chunk_dims[j] = chunk_dims[j] - n_elems_beyond_edge;
                    }
                    else
                        partial_chunk_dims[j] = chunk_dims[j];
                }
            }

            /* "AND" temporary chunk and current chunk */
            if (H5Sselect_hyperslab(tmp_chunk_fspace_id, H5S_SELECT_AND, start_coords, NULL,
                    is_partial_edge_chunk ? partial_chunk_dims : chunk_dims, NULL) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTSELECT, FAIL, "can't create temporary chunk selection");

            /* Resize chunk's dataspace dimensions to size of chunk */
            if (H5Sset_extent_simple(tmp_chunk_fspace_id, fspace_ndims, chunk_dims, NULL) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTSELECT, FAIL, "can't adjust chunk dimensions");

            /* Move selection back to have correct offset in chunk */
            if (H5Sselect_adjust_u(tmp_chunk_fspace_id, start_coords) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTSELECT, FAIL, "can't adjust chunk selection");

            /* Copy the chunk's coordinates to the selected chunk info buffer */
            memcpy(_chunk_info[i].chunk_coords, start_coords, (size_t) fspace_ndims * sizeof(hsize_t));

            _chunk_info[i].fspace_id = tmp_chunk_fspace_id;

            /*
             * Now set up the memory Dataspace for this chunk.
             */
            if (space_same_shape) {
                if ((tmp_chunk_mspace_id = H5Scopy(mem_space_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTCOPY, FAIL, "unable to copy memory space");

                /* NOTE: Should not be needed - H5Scopy should release the selection, but there is currently a bug in HDF5 */
                /* Release the current selection */
                if (H5Sselect_release(tmp_chunk_mspace_id) < 0)
                    D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to release selection in temporary chunk's memory dataspace");

                /* Copy the chunk's file space selection to its memory space selection */
                if (H5Sselect_copy(tmp_chunk_mspace_id, tmp_chunk_fspace_id) < 0)
                    D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTCOPY, FAIL, "unable to copy selection from temporary chunk's file dataspace to its memory dataspace");

                /* Compute the adjustment for the chunk */
                for (j = 0; j < (size_t) fspace_ndims; j++) {
                    /* H5_CHECK_OVERFLOW(_chunk_info[i].chunk_coords[j], hsize_t, hssize_t); */
                    chunk_mem_space_adjust[j] = chunk_file_space_adjust[j] - (hssize_t) _chunk_info[i].chunk_coords[j];
                } /* end for */

                /* Adjust the selection */
                if (H5Shyper_adjust_s(tmp_chunk_mspace_id, chunk_mem_space_adjust) < 0)
                    D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTSELECT, FAIL, "can't adjust temporary chunk's memory space selection");

                _chunk_info[i].mspace_id = tmp_chunk_mspace_id;
            } /* end if */
            else {
                D_GOTO_ERROR(H5E_ARGS, H5E_UNSUPPORTED, FAIL, "file and memory selections must currently have the same shape");
            } /* end else */

            i++;

            /* Determine if there are more chunks to process */
            if ((chunk_sel_npoints = H5Sget_select_npoints(tmp_chunk_fspace_id)) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "can't get number of points selected in chunk file space");

            /*
             * Just to be safe against an underflow..
             */
            if ((hsize_t) chunk_sel_npoints > num_sel_points_cast)
                num_sel_points_cast = 0;
            else
                num_sel_points_cast -= (hsize_t) chunk_sel_npoints;

            if (num_sel_points_cast == 0)
                D_GOTO_DONE(SUCCEED);
        } /* end if */

        /* Set current increment dimension */
        increment_dim = fspace_ndims - 1;

        /* Increment chunk location in fastest changing dimension */
        /* H5_CHECK_OVERFLOW(chunk_dims[increment_dim], hsize_t, hssize_t); */
        start_coords[increment_dim] += chunk_dims[increment_dim];
        end_coords[increment_dim] += chunk_dims[increment_dim];

        /* Bring chunk location back into bounds, if necessary */
        if (start_coords[increment_dim] > file_sel_end[increment_dim]) {
            do {
                /* Reset current dimension's location to 0 */
                start_coords[increment_dim] = selection_start_coords[increment_dim];
                end_coords[increment_dim] = (start_coords[increment_dim] + chunk_dims[increment_dim]) - 1;

                /* Decrement current dimension */
                assert(increment_dim > 0);
                increment_dim--;

                /* Increment chunk location in current dimension */
                start_coords[increment_dim] += chunk_dims[increment_dim];
                end_coords[increment_dim] = (start_coords[increment_dim] + chunk_dims[increment_dim]) - 1;
            } while (start_coords[increment_dim] > file_sel_end[increment_dim]);
        } /* end if */
    } /* end for */

done:
    if (ret_value < 0) {
        if (_chunk_info) {
            for (j = 0; j < i; j++) {
                if ((_chunk_info[j].fspace_id >= 0) && (H5Sclose(_chunk_info[j].fspace_id) < 0))
                    D_DONE_ERROR(H5E_DATASPACE, H5E_CANTCLOSEOBJ, FAIL, "failed to close chunk file dataspace ID")
                if ((_chunk_info[j].mspace_id >= 0) && (H5Sclose(_chunk_info[j].mspace_id) < 0))
                    D_DONE_ERROR(H5E_DATASPACE, H5E_CANTCLOSEOBJ, FAIL, "failed to close chunk memory dataspace ID")
            }

            DV_free(_chunk_info);
        }

        /*
         * Make sure to close the temp. chunk space IDs in case we failed
         * before assigning them to a piece of the chunk info array.
         */
        H5E_BEGIN_TRY {
            H5Sclose(tmp_chunk_fspace_id);
            H5Sclose(tmp_chunk_mspace_id);
        } H5E_END_TRY;
    }
    else {
        *chunk_info = _chunk_info;
        *chunk_info_len = i;
    }

    D_FUNC_LEAVE
} /* end H5_daos_get_selected_chunk_info() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_is_partial_edge_chunk
 *
 * Purpose:     Determines whether a given chunk is a partial edge chunk,
 *              based on the chunk's coordinates in relation to the given
 *              dataset dimensions.
 *
 * Return:      Success: TRUE/FALSE (can't fail)
 *
 * Programmer:  Jordan Henderson
 *              July, 2019
 *
 *-------------------------------------------------------------------------
 */
static hbool_t
H5_daos_is_partial_edge_chunk(unsigned dims_rank, const hsize_t *dset_dims,
    const hsize_t *chunk_dims, const hsize_t *chunk_coords)
{
    unsigned i;

    assert(dims_rank > 0);
    assert(dset_dims);
    assert(chunk_dims);
    assert(chunk_coords);

    for (i = 0; i < dims_rank; i++)
        if (chunk_coords[i] + chunk_dims[i] > dset_dims[i])
            return TRUE;

    return FALSE;
} /* end H5_daos_is_partial_edge_chunk() */
