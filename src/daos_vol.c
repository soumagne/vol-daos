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
 *          library.  General connector routines.
 */

#include "daos_vol.h"           /* DAOS connector                          */

#include "util/daos_vol_err.h"  /* DAOS connector error handling           */
#include "util/daos_vol_mem.h"  /* DAOS connector memory management        */

#include <daos_mgmt.h>          /* For pool creation */

/* HDF5 header for dynamic plugin loading */
#include <H5PLextern.h>

/****************/
/* Local Macros */
/****************/

/* Default DAOS group ID used for creating pools */
#ifndef DAOS_DEFAULT_GROUP_ID
# define DAOS_DEFAULT_GROUP_ID "daos_server"
#endif
#define H5_DAOS_MAX_GRP_NAME     64
#define H5_DAOS_MAX_SVC_REPLICAS 13

/* Macro to "allocate" the next OIDX value from the local allocation of OIDXs */
#define H5_DAOS_ALLOCATE_NEXT_OIDX(oidx_out_ptr, next_oidx_ptr, max_oidx_ptr) \
do {                                                                          \
    assert((*next_oidx_ptr) <= (*max_oidx_ptr));                              \
    (*oidx_out_ptr) = (*next_oidx_ptr);                                       \
    (*next_oidx_ptr)++;                                                       \
} while(0)

/* Macro to adjust the next OIDX and max. OIDX pointers after
 * allocating more OIDXs from DAOS.
 */
#define H5_DAOS_ADJUST_MAX_AND_NEXT_OIDX(next_oidx_ptr, max_oidx_ptr) \
do {                                                                  \
    /* Set max oidx */                                                \
    (*max_oidx_ptr) = (*next_oidx_ptr) + H5_DAOS_OIDX_NALLOC - 1;     \
                                                                      \
    /* Skip over reserved indices for the next oidx */                \
    assert(H5_DAOS_OIDX_NALLOC > H5_DAOS_OIDX_FIRST_USER);            \
    if((*next_oidx_ptr) < H5_DAOS_OIDX_FIRST_USER)                    \
        (*next_oidx_ptr) = H5_DAOS_OIDX_FIRST_USER;                   \
} while(0)

#define H5_DAOS_PRINT_UUID(uuid) do {       \
    char uuid_buf[37];                      \
    uuid_unparse(uuid, uuid_buf);           \
    printf("POOL UUID = %s\n", uuid_buf);   \
} while (0)

/************************************/
/* Local Type and Struct Definition */
/************************************/

/* Task user data for pool connect */
typedef struct H5_daos_pool_connect_ud_t {
    H5_daos_req_t *req;
    const uuid_t *puuid;
    daos_handle_t *poh;
    daos_pool_info_t *info;
    const char *grp;
    d_rank_list_t *svc;
    unsigned int flags;
    hbool_t free_rank_list;
} H5_daos_pool_connect_ud_t;

/* Task user data for pool disconnect */
typedef struct H5_daos_pool_disconnect_ud_t {
    H5_daos_req_t *req;
    daos_handle_t *poh;
} H5_daos_pool_disconnect_ud_t;

typedef struct H5_daos_pool_query_ud_t {
    H5_daos_generic_cb_ud_t generic_ud; /* Must be first */
    daos_handle_t *poh;
    daos_pool_info_t *pool_info;
    d_rank_list_t *tgts;
    daos_prop_t *prop;
} H5_daos_pool_query_ud_t;

/* Task user data for DAOS object open */
typedef struct H5_daos_obj_open_ud_t {
    H5_daos_generic_cb_ud_t generic_ud; /* Must be first */
    H5_daos_file_t *file;
    daos_obj_id_t *oid;
} H5_daos_obj_open_ud_t;

typedef struct H5_daos_pool_create_info {
    uuid_t pool_uuid;
    d_rank_list_t svcl;
} H5_daos_pool_create_info;

/********************/
/* Local Prototypes */
/********************/

static herr_t H5_daos_set_object_class(hid_t plist_id, char *object_class);
static herr_t H5_daos_str_prop_delete(hid_t prop_id, const char *name,
    size_t size, void *_value);
static herr_t H5_daos_str_prop_copy(const char *name, size_t size,
    void *_value);
static int H5_daos_str_prop_compare(const void *_value1, const void *_value2,
    size_t size);
static herr_t H5_daos_str_prop_close(const char *name, size_t size,
    void *_value);
static int H5_daos_bool_prop_compare(const void *_value1, const void *_value2,
    size_t size);
static herr_t H5_daos_init(hid_t vipl_id);
static herr_t H5_daos_term(void);
static herr_t H5_daos_set_pool_globals(uuid_t pool_uuid, const char *pool_grp, const char *pool_svcl);
static herr_t H5_daos_pool_create_bcast(uuid_t pool_uuid, d_rank_list_t *pool_svcl,
    MPI_Comm comm, int rank);
static void *H5_daos_fapl_copy(const void *_old_fa);
static herr_t H5_daos_fapl_free(void *_fa);
static herr_t H5_daos_get_conn_cls(void *item, H5VL_get_conn_lvl_t lvl,
    const H5VL_class_t **conn_cls);
static herr_t H5_daos_opt_query(void *item, H5VL_subclass_t cls, int opt_type,
    hbool_t *supported);
static herr_t H5_daos_optional(void *item, int op_type, hid_t dxpl_id,
    void **req, va_list arguments);

static herr_t H5_daos_oidx_bcast(H5_daos_file_t *file, uint64_t *oidx_out,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task);
static int H5_daos_oidx_bcast_prep_cb(tse_task_t *task, void *args);
static int H5_daos_oidx_bcast_comp_cb(tse_task_t *task, void *args);
static int H5_daos_oidx_generate_comp_cb(tse_task_t *task, void *args);
static int H5_daos_oid_encode_task(tse_task_t *task);
static int H5_daos_list_key_prep_cb(tse_task_t *task, void *args);
static int H5_daos_list_key_finish(tse_task_t *task);
static int H5_daos_free_async_task(tse_task_t *task);
static int H5_daos_pool_connect_prep_cb(tse_task_t *task, void *args);
static int H5_daos_pool_connect_comp_cb(tse_task_t *task, void *args);
static int H5_daos_pool_disconnect_prep_cb(tse_task_t *task, void *args);
static int H5_daos_pool_disconnect_comp_cb(tse_task_t *task, void *args);
static int H5_daos_pool_query_prep_cb(tse_task_t *task, void *args);
static int H5_daos_sched_link_old_task(tse_task_t *task);

static int H5_daos_collective_error_check_prep_cb(tse_task_t *task, void *args);
static int H5_daos_collective_error_check_comp_cb(tse_task_t *task, void *args);

/*******************/
/* Local Variables */
/*******************/

/* The DAOS VOL connector struct */
static const H5VL_class_t H5_daos_g = {
    HDF5_VOL_DAOS_VERSION_1,                 /* Plugin Version number */
    H5_VOL_DAOS_CLS_VAL,                     /* Plugin Value */
    H5_DAOS_VOL_NAME,                        /* Plugin Name */
    H5VL_CAP_FLAG_NONE,                      /* Plugin capability flags */
    H5_daos_init,                            /* Plugin initialize */
    H5_daos_term,                            /* Plugin terminate */
    {
        sizeof(H5_daos_fapl_t),              /* Plugin Info size */
        H5_daos_fapl_copy,                   /* Plugin Info copy */
        NULL,                                /* Plugin Info compare */
        H5_daos_fapl_free,                   /* Plugin Info free */
        NULL,                                /* Plugin Info To String */
        NULL,                                /* Plugin String To Info */
    },
    {
        NULL,                                /* Plugin Get Object */
        NULL,                                /* Plugin Get Wrap Ctx */
        NULL,                                /* Plugin Wrap Object */
        NULL,                                /* Plugin Unwrap Object */
        NULL,                                /* Plugin Free Wrap Ctx */
    },
    {                                        /* Plugin Attribute cls */
        H5_daos_attribute_create,            /* Plugin Attribute create */
        H5_daos_attribute_open,              /* Plugin Attribute open */
        H5_daos_attribute_read,              /* Plugin Attribute read */
        H5_daos_attribute_write,             /* Plugin Attribute write */
        H5_daos_attribute_get,               /* Plugin Attribute get */
        H5_daos_attribute_specific,          /* Plugin Attribute specific */
        NULL,                                /* Plugin Attribute optional */
        H5_daos_attribute_close              /* Plugin Attribute close */
    },
    {                                        /* Plugin Dataset cls */
        H5_daos_dataset_create,              /* Plugin Dataset create */
        H5_daos_dataset_open,                /* Plugin Dataset open */
        H5_daos_dataset_read,                /* Plugin Dataset read */
        H5_daos_dataset_write,               /* Plugin Dataset write */
        H5_daos_dataset_get,                 /* Plugin Dataset get */
        H5_daos_dataset_specific,            /* Plugin Dataset specific */
        NULL,                                /* Plugin Dataset optional */
        H5_daos_dataset_close                /* Plugin Dataset close */
    },
    {                                        /* Plugin Datatype cls */
        H5_daos_datatype_commit,             /* Plugin Datatype commit */
        H5_daos_datatype_open,               /* Plugin Datatype open */
        H5_daos_datatype_get,                /* Plugin Datatype get */
        H5_daos_datatype_specific,           /* Plugin Datatype specific */
        NULL,                                /* Plugin Datatype optional */
        H5_daos_datatype_close               /* Plugin Datatype close */
    },
    {                                        /* Plugin File cls */
        H5_daos_file_create,                 /* Plugin File create */
        H5_daos_file_open,                   /* Plugin File open */
        H5_daos_file_get,                    /* Plugin File get */
        H5_daos_file_specific,               /* Plugin File specific */
        NULL,                                /* Plugin File optional */
        H5_daos_file_close                   /* Plugin File close */
    },
    {                                        /* Plugin Group cls */
        H5_daos_group_create,                /* Plugin Group create */
        H5_daos_group_open,                  /* Plugin Group open */
        H5_daos_group_get,                   /* Plugin Group get */
        H5_daos_group_specific,              /* Plugin Group specific */
        NULL,                                /* Plugin Group optional */
        H5_daos_group_close                  /* Plugin Group close */
    },
    {                                        /* Plugin Link cls */
        H5_daos_link_create,                 /* Plugin Link create */
        H5_daos_link_copy,                   /* Plugin Link copy */
        H5_daos_link_move,                   /* Plugin Link move */
        H5_daos_link_get,                    /* Plugin Link get */
        H5_daos_link_specific,               /* Plugin Link specific */
        NULL                                 /* Plugin Link optional */
    },
    {                                        /* Plugin Object cls */
        H5_daos_object_open,                 /* Plugin Object open */
        H5_daos_object_copy,                 /* Plugin Object copy */
        H5_daos_object_get,                  /* Plugin Object get */
        H5_daos_object_specific,             /* Plugin Object specific */
        NULL                                 /* Plugin Object optional */
    },
    {
        H5_daos_get_conn_cls,                /* Plugin get connector class */
        H5_daos_opt_query                    /* Plugin optional callback query */
    },
    {
        H5_daos_req_wait,                    /* Plugin Request wait */
        H5_daos_req_notify,                  /* Plugin Request notify */
        H5_daos_req_cancel,                  /* Plugin Request cancel */
        NULL,                                /* Plugin Request specific */
        NULL,                                /* Plugin Request optional */
        H5_daos_req_free                     /* Plugin Request free */
    },
    {
        H5_daos_blob_put,                    /* Plugin 'blob' put */
        H5_daos_blob_get,                    /* Plugin 'blob' get */
        H5_daos_blob_specific,               /* Plugin 'blob' specific */
        NULL                                 /* Plugin 'blob' optional */
    },
    {
        NULL,                                /* Plugin Token compare */
        NULL,                                /* Plugin Token to string */
        NULL                                 /* Plugin Token from string */
    },
    H5_daos_optional                         /* Plugin optional */
};

/* Free list definitions */
/* DSINC - currently no external access to free lists
H5FL_DEFINE(H5_daos_file_t);
H5FL_DEFINE(H5_daos_group_t);
H5FL_DEFINE(H5_daos_dset_t);
H5FL_DEFINE(H5_daos_dtype_t);
H5FL_DEFINE(H5_daos_map_t);
H5FL_DEFINE(H5_daos_attr_t);*/

hid_t H5_DAOS_g = H5I_INVALID_HID;
static hbool_t H5_daos_initialized_g = FALSE;

/* Identifiers for HDF5's error API */
hid_t dv_err_stack_g = H5I_INVALID_HID;
hid_t dv_err_class_g = H5I_INVALID_HID;
hid_t dv_obj_err_maj_g = H5I_INVALID_HID;
hid_t dv_async_err_g = H5I_INVALID_HID;

#ifdef DV_TRACK_MEM_USAGE
/*
 * Counter to keep track of the currently allocated amount of bytes
 */
size_t daos_vol_curr_alloc_bytes;
#endif

/* Global variables used to connect to DAOS pools */
static hbool_t H5_daos_pool_globals_set_g = FALSE;  /* Pool config set */
uuid_t  H5_daos_pool_uuid_g;                        /* Pool UUID */
char H5_daos_pool_grp_g[H5_DAOS_MAX_GRP_NAME + 1] = {'\0'}; /* Pool Group */
static d_rank_t H5_daos_pool_ranks_g[H5_DAOS_MAX_SVC_REPLICAS]; /* Pool ranks */
d_rank_list_t H5_daos_pool_svcl_g = {0};                  /* Pool svc list */
static const unsigned int   H5_daos_pool_default_mode_g          = 0731;         /* Default Mode */
static const daos_size_t    H5_daos_pool_default_scm_size_g      = (1ULL << 31); /*   2GB */
static const daos_size_t    H5_daos_pool_default_nvme_size_g     = (1ULL << 33); /*   8GB */
static const unsigned int   H5_daos_pool_default_svc_nreplicas_g = 1;            /* Number of replicas */

/* Global variable used to bypass the DUNS in favor of standard DAOS
 * container operations if requested.
 */
hbool_t H5_daos_bypass_duns_g = FALSE;

/* Target chunk size for automatic chunking */
uint64_t H5_daos_chunk_target_size_g = H5_DAOS_CHUNK_TARGET_SIZE_DEF;

/* DAOS task and MPI request for current in-flight MPI operation */
tse_task_t *H5_daos_mpi_task_g = NULL;
MPI_Request H5_daos_mpi_req_g;

/* Constant Keys */
const char H5_daos_int_md_key_g[]          = "/Internal Metadata";
const char H5_daos_root_grp_oid_key_g[]    = "Root Group OID";
const char H5_daos_rc_key_g[]              = "Ref Count";
const char H5_daos_cpl_key_g[]             = "Creation Property List";
const char H5_daos_link_key_g[]            = "Link";
const char H5_daos_link_corder_key_g[]     = "/Link Creation Order";
const char H5_daos_nlinks_key_g[]          = "Num Links";
const char H5_daos_max_link_corder_key_g[] = "Max Link Creation Order";
const char H5_daos_type_key_g[]            = "Datatype";
const char H5_daos_space_key_g[]           = "Dataspace";
const char H5_daos_attr_key_g[]            = "/Attribute";
const char H5_daos_nattr_key_g[]           = "Num Attributes";
const char H5_daos_max_attr_corder_key_g[] = "Max Attribute Creation Order";
const char H5_daos_ktype_g[]               = "Key Datatype";
const char H5_daos_vtype_g[]               = "Value Datatype";
const char H5_daos_map_key_g[]             = "Map Record";
const char H5_daos_blob_key_g[]            = "Blob";
const char H5_daos_fillval_key_g[]         = "Fill Value";

const daos_size_t H5_daos_int_md_key_size_g          = (daos_size_t)(sizeof(H5_daos_int_md_key_g) - 1);
const daos_size_t H5_daos_root_grp_oid_key_size_g    = (daos_size_t)(sizeof(H5_daos_root_grp_oid_key_g) - 1);
const daos_size_t H5_daos_rc_key_size_g              = (daos_size_t)(sizeof(H5_daos_rc_key_g) - 1);
const daos_size_t H5_daos_cpl_key_size_g             = (daos_size_t)(sizeof(H5_daos_cpl_key_g) - 1);
const daos_size_t H5_daos_link_key_size_g            = (daos_size_t)(sizeof(H5_daos_link_key_g) - 1);
const daos_size_t H5_daos_link_corder_key_size_g     = (daos_size_t)(sizeof(H5_daos_link_corder_key_g) - 1);
const daos_size_t H5_daos_nlinks_key_size_g          = (daos_size_t)(sizeof(H5_daos_nlinks_key_g) - 1);
const daos_size_t H5_daos_max_link_corder_key_size_g = (daos_size_t)(sizeof(H5_daos_max_link_corder_key_g) - 1);
const daos_size_t H5_daos_type_key_size_g            = (daos_size_t)(sizeof(H5_daos_type_key_g) - 1);
const daos_size_t H5_daos_space_key_size_g           = (daos_size_t)(sizeof(H5_daos_space_key_g) - 1);
const daos_size_t H5_daos_attr_key_size_g            = (daos_size_t)(sizeof(H5_daos_attr_key_g) - 1);
const daos_size_t H5_daos_nattr_key_size_g           = (daos_size_t)(sizeof(H5_daos_nattr_key_g) - 1);
const daos_size_t H5_daos_max_attr_corder_key_size_g = (daos_size_t)(sizeof(H5_daos_max_attr_corder_key_g) - 1);
const daos_size_t H5_daos_ktype_size_g               = (daos_size_t)(sizeof(H5_daos_ktype_g) - 1);
const daos_size_t H5_daos_vtype_size_g               = (daos_size_t)(sizeof(H5_daos_vtype_g) - 1);
const daos_size_t H5_daos_map_key_size_g             = (daos_size_t)(sizeof(H5_daos_map_key_g) - 1);
const daos_size_t H5_daos_blob_key_size_g            = (daos_size_t)(sizeof(H5_daos_blob_key_g) - 1);
const daos_size_t H5_daos_fillval_key_size_g         = (daos_size_t)(sizeof(H5_daos_fillval_key_g) - 1);


/*-------------------------------------------------------------------------
 * Function:    H5daos_init
 *
 * Purpose:     Initialize this VOL connector by connecting to the pool and
 *              registering the connector with the library.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              March, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_init(uuid_t pool_uuid, const char *pool_grp, const char *pool_svcl)
{
    H5I_type_t idType = H5I_UNINIT;
    herr_t     ret_value = SUCCEED;            /* Return value */

    if(uuid_is_null(pool_uuid))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "not a valid UUID");
    if(NULL == pool_grp)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "not a valid service group");
    if(NULL == pool_svcl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "not a valid service list");

    /* Initialize HDF5 */
    if(H5open() < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "HDF5 failed to initialize");

    if(H5_DAOS_g >= 0 && (idType = H5Iget_type(H5_DAOS_g)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "failed to retrieve DAOS VOL connector's ID type");

    /* Register the DAOS VOL, if it isn't already */
    if(H5I_VOL != idType) {
        htri_t is_registered;

        if((is_registered = H5VLis_connector_registered_by_value(H5_daos_g.value)) < 0)
            D_GOTO_ERROR(H5E_ATOM, H5E_CANTINIT, FAIL, "can't determine if DAOS VOL connector is registered");

        if(!is_registered) {
            /* Save arguments to globals */
            if(H5_daos_set_pool_globals(pool_uuid, pool_grp, pool_svcl) < 0)
                D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't set pool globals");

            /* Register connector */
            if((H5_DAOS_g = H5VLregister_connector((const H5VL_class_t *)&H5_daos_g, H5P_DEFAULT)) < 0)
                D_GOTO_ERROR(H5E_ATOM, H5E_CANTINSERT, FAIL, "can't create ID for DAOS VOL connector");
        } /* end if */
        else {
            if((H5_DAOS_g = H5VLget_connector_id_by_name(H5_daos_g.name)) < 0)
                D_GOTO_ERROR(H5E_ATOM, H5E_CANTGET, FAIL, "unable to get registered ID for DAOS VOL connector");
        } /* end else */
    } /* end if */

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_init() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_term
 *
 * Purpose:     Shut down the DAOS VOL
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              March, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_term(void)
{
    herr_t ret_value = SUCCEED;            /* Return value */

    /* Terminate the connector */
    if(H5_daos_term() < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't terminate DAOS VOL connector");

done:
#ifdef DV_TRACK_MEM_USAGE
    /* Check for allocated memory */
    if(0 != daos_vol_curr_alloc_bytes)
        FUNC_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "%zu bytes were still left allocated", daos_vol_curr_alloc_bytes)

    daos_vol_curr_alloc_bytes = 0;
#endif

    /* Unregister from the HDF5 error API */
    if(dv_err_class_g >= 0) {
        if(dv_obj_err_maj_g >= 0 && H5Eclose_msg(dv_obj_err_maj_g) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't unregister error message for object interface");
        if(dv_async_err_g >= 0 && H5Eclose_msg(dv_async_err_g) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't unregister error message for asynchronous interface");

        if(H5Eunregister_class(dv_err_class_g) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't unregister error class from HDF5 error API");

        /* Print the current error stack before destroying it */
        PRINT_ERROR_STACK;

        /* Destroy the error stack */
        if(H5Eclose_stack(dv_err_stack_g) < 0) {
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't close HDF5 error stack");
            PRINT_ERROR_STACK;
        } /* end if */

        dv_err_stack_g = H5I_INVALID_HID;
        dv_err_class_g = H5I_INVALID_HID;
        dv_obj_err_maj_g = H5I_INVALID_HID;
        dv_async_err_g = H5I_INVALID_HID;
    } /* end if */

    D_FUNC_LEAVE_API;
} /* end H5daos_term() */


/*-------------------------------------------------------------------------
 * Function:    H5Pset_fapl_daos
 *
 * Purpose:     Modify the file access property list to use the DAOS VOL
 *              connector defined in this source file.  file_comm and
 *              file_info identify the communicator and info object used
 *              to coordinate actions on file create, open, flush, and
 *              close.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5Pset_fapl_daos(hid_t fapl_id, MPI_Comm file_comm, MPI_Info file_info)
{
    H5_daos_fapl_t fa;
    htri_t         is_fapl;
    herr_t         ret_value = FAIL;

    if(H5_DAOS_g < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_UNINITIALIZED, FAIL, "DAOS VOL connector not initialized");

    if(fapl_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list");

    if((is_fapl = H5Pisa_class(fapl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if(!is_fapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list");

    if(MPI_COMM_NULL == file_comm)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADTYPE, FAIL, "not a valid MPI communicator");

    /* Initialize driver specific properties */
    fa.comm = file_comm;
    fa.info = file_info;
    fa.free_comm_info = FALSE;

    ret_value = H5Pset_vol(fapl_id, H5_DAOS_g, &fa);

done:
    D_FUNC_LEAVE_API;
} /* end H5Pset_fapl_daos() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_set_object_class
 *
 * Purpose:     Sets the provided DAOS object class on the property list.
 *              See DAOS documentation for a list of object classes and
 *              descriptions of them.
 *
 *              If called on a FCPL, GCPL, TCPL, DCPL, or MCPL, it affects
 *              objects created using that creation property list (FCPL
 *              affects only the file root group and global metadata
 *              object).
 *
 *              If called on a FAPL it affects all objects created during
 *              this file open, except those with their object class
 *              specified via the creation property list, as above.
 *
 *              The default value is "", which allows the connector to set
 *              the object class according to its default for the object
 *              type.
 *
 *              If the root group is created with a non-default object
 *              class, then if the file is opened at a later time, the
 *              root group's object class must the be set on the FAPL
 *              using H5daos_set_root_open_object_class().
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_set_object_class(hid_t plist_id, char *object_class)
{
    herr_t      ret_value = SUCCEED;

    if(plist_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list");

    /* Call internal routine */
    if(H5_daos_set_object_class(plist_id, object_class) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTSET, FAIL, "can't set object class");

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_set_object_class() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_set_object_class
 *
 * Purpose:     Internal version of H5daos_set_object_class().
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_set_object_class(hid_t plist_id, char *object_class)
{
    char        *copied_object_class = NULL;
    htri_t      prop_exists;
    herr_t      ret_value = SUCCEED;

    /* Check if the property already exists on the property list */
    if((prop_exists = H5Pexist(plist_id, H5_DAOS_OBJ_CLASS_NAME)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for object class property");

    /* Copy object class */
    if(object_class)
        if(NULL == (copied_object_class = strdup(object_class)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't copy object class string");

    /* Set the property, or insert it if it does not exist */
    if(prop_exists) {
        if(H5Pset(plist_id, H5_DAOS_OBJ_CLASS_NAME, &copied_object_class) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "can't set property");
    } /* end if */
    else
        if(H5Pinsert2(plist_id, H5_DAOS_OBJ_CLASS_NAME, sizeof(char *),
                &copied_object_class, NULL, NULL,
                H5_daos_str_prop_delete, H5_daos_str_prop_copy,
                H5_daos_str_prop_compare, H5_daos_str_prop_close) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTINSERT, FAIL, "can't insert property into list");

done:
    D_FUNC_LEAVE;
} /* end H5_daos_set_object_class() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_set_oclass_from_oid
 *
 * Purpose:     Decodes the object class embedded in the provided DAOS OID
 *              and adds it to the provided property list.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_set_oclass_from_oid(hid_t plist_id, daos_obj_id_t oid)
{
    daos_oclass_id_t oc_id;
    char oclass_str[10]; /* DAOS uses a size of 10 internally for these calls */
    herr_t ret_value = SUCCEED;

    /* Get object class id from oid */
    /* Replace with DAOS function once public! DSINC */
    oc_id = (oid.hi & OID_FMT_CLASS_MASK) >> OID_FMT_CLASS_SHIFT;

    /* Get object class string */
    if(daos_oclass_id2name(oc_id, oclass_str) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't get object class string");

    /* Set object class string on plist */
    if(H5_daos_set_object_class(plist_id, oclass_str) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTSET, FAIL, "can't set object class");

done:
    D_FUNC_LEAVE;
} /* end H5_daos_set_oclass_from_oid() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_get_object_class
 *
 * Purpose:     Retrieves the object class from the provided property
 *              list.  If plist_id was retrieved via a call to
 *              H5*get_create_plist(), the returned object class will be
 *              the actual DAOS object class of the object (it will not be
 *              the property list default value of "").
 *
 *              If not NULL, object_class points to a user-allocated
 *              output buffer, whose size is size.
 *
 * Return:      Success:        length of object class string (excluding
 *                              null terminator)
 *              Failure:        -1
 *
 *-------------------------------------------------------------------------
 */
ssize_t
H5daos_get_object_class(hid_t plist_id, char *object_class, size_t size)
{
    char        *tmp_object_class = NULL;
    htri_t      prop_exists;
    size_t      len;
    ssize_t     ret_value;

    /* Check if the property already exists on the property list */
    if((prop_exists = H5Pexist(plist_id, H5_DAOS_OBJ_CLASS_NAME)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for object class property");

    if(prop_exists) {
        /* Get the property */
        if(H5Pget(plist_id, H5_DAOS_OBJ_CLASS_NAME, &tmp_object_class) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get object class");

        /* Set output values */
        if(tmp_object_class) {
            len = strlen(tmp_object_class);
            if(object_class && (size > 0)) {
                strncpy(object_class, tmp_object_class, size);
                if(len >= size)
                    object_class[size - 1] = '\0';
            } /* end if */
        } /* end if */
        else {
            /* Simply return an empty string */
            len = 0;
            if(object_class && (size > 0))
                object_class[0] = '\0';
        } /* end else */
    } /* end if */
    else {
        /* Simply return an empty string */
        len = 0;
        if(object_class && (size > 0))
            object_class[0] = '\0';
    } /* end else */

    /* Set return value */
    ret_value = (ssize_t)len;

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_get_object_class() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_set_root_open_object_class
 *
 * Purpose:     Sets the object class to use for opening the root group on
 *              the provided file access property list.  This should match
 *              the object class used to create the root group via
 *              H5daos_set_object_class().
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_set_root_open_object_class(hid_t fapl_id, char *object_class)
{
    htri_t      is_fapl;
    char        *copied_object_class = NULL;
    htri_t      prop_exists;
    herr_t      ret_value = SUCCEED;

    if(fapl_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list");

    if((is_fapl = H5Pisa_class(fapl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if(!is_fapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list");

    /* Check if the property already exists on the property list */
    if((prop_exists = H5Pexist(fapl_id, H5_DAOS_ROOT_OPEN_OCLASS_NAME)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for object class property");

    /* Copy object class */
    if(object_class)
        if(NULL == (copied_object_class = strdup(object_class)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't copy object class string");

    /* Set the property, or insert it if it does not exist */
    if(prop_exists) {
        if(H5Pset(fapl_id, H5_DAOS_ROOT_OPEN_OCLASS_NAME, &copied_object_class) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "can't set property");
    } /* end if */
    else
        if(H5Pinsert2(fapl_id, H5_DAOS_ROOT_OPEN_OCLASS_NAME, sizeof(char *),
                &copied_object_class, NULL, NULL,
                H5_daos_str_prop_delete, H5_daos_str_prop_copy,
                H5_daos_str_prop_compare, H5_daos_str_prop_close) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTINSERT, FAIL, "can't insert property into list");

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_set_root_open_object_class() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_get_root_open_object_class
 *
 * Purpose:     Retrieves the object class for opening the root group from
 *              the provided file access property list, as set by
 *              H5daos_set_root_open_object_class().
 *
 *              If not NULL, object_class points to a user-allocated
 *              output buffer, whose size is size.
 *
 * Return:      Success:        length of object class string (excluding
 *                              null terminator)
 *              Failure:        -1
 *
 *-------------------------------------------------------------------------
 */
ssize_t
H5daos_get_root_open_object_class(hid_t fapl_id, char *object_class, size_t size)
{
    htri_t      is_fapl;
    char        *tmp_object_class = NULL;
    htri_t      prop_exists;
    size_t      len;
    ssize_t     ret_value;

    if((is_fapl = H5Pisa_class(fapl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if(!is_fapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list");

    /* Check if the property already exists on the property list */
    if((prop_exists = H5Pexist(fapl_id, H5_DAOS_ROOT_OPEN_OCLASS_NAME)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for object class property");

    if(prop_exists) {
        /* Get the property */
        if(H5Pget(fapl_id, H5_DAOS_ROOT_OPEN_OCLASS_NAME, &tmp_object_class) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get object class");

        /* Set output values */
        if(tmp_object_class) {
            len = strlen(tmp_object_class);
            if(object_class && (size > 0)) {
                strncpy(object_class, tmp_object_class, size);
                if(len >= size)
                    object_class[size - 1] = '\0';
            } /* end if */
        } /* end if */
        else {
            /* Simply return an empty string */
            len = 0;
            if(object_class && (size > 0))
                object_class[0] = '\0';
        } /* end else */
    } /* end if */
    else {
        /* Simply return an empty string */
        len = 0;
        if(object_class && (size > 0))
            object_class[0] = '\0';
    } /* end else */

    /* Set return value */
    ret_value = (ssize_t)len;

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_get_root_open_object_class() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_set_all_ind_metadata_ops
 *
 * Purpose:     Modifies the access property list to indicate that all
 *              metadata I/O operations should be performed independently.
 *              By default, metadata reads are independent and metadata
 *              writes are collective.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_set_all_ind_metadata_ops(hid_t accpl_id, hbool_t is_independent)
{
    htri_t is_fapl;
    htri_t is_lapl;
    htri_t is_rapl;
    htri_t prop_exists;
    herr_t ret_value = SUCCEED;

    if(accpl_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list");

    if((is_fapl = H5Pisa_class(accpl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if((is_lapl = H5Pisa_class(accpl_id, H5P_LINK_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if((is_rapl = H5Pisa_class(accpl_id, H5P_REFERENCE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if(!is_fapl && !is_lapl && !is_rapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not an access property list");

    /* Check if the independent metadata writes property already exists on the property list */
    if((prop_exists = H5Pexist(accpl_id, H5_DAOS_IND_MD_IO_PROP_NAME)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for independent metadata I/O property");

    /* Set the property, or insert it if it does not exist */
    if(prop_exists) {
        if(H5Pset(accpl_id, H5_DAOS_IND_MD_IO_PROP_NAME, &is_independent) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "can't set independent metadata I/O property");
    } /* end if */
    else
        if(H5Pinsert2(accpl_id, H5_DAOS_IND_MD_IO_PROP_NAME, sizeof(hbool_t),
                &is_independent, NULL, NULL, NULL, NULL,
                H5_daos_bool_prop_compare, NULL) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTINSERT, FAIL, "can't insert property into list");

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_set_all_ind_metadata_ops() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_get_all_ind_metadata_ops
 *
 * Purpose:     Retrieves the independent metadata I/O setting from the
 *              access property list accpl_id.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_get_all_ind_metadata_ops(hid_t accpl_id, hbool_t *is_independent)
{
    htri_t is_fapl;
    htri_t is_lapl;
    htri_t is_rapl;
    htri_t prop_exists;
    herr_t ret_value = SUCCEED;

    if((is_fapl = H5Pisa_class(accpl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if((is_lapl = H5Pisa_class(accpl_id, H5P_LINK_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if((is_rapl = H5Pisa_class(accpl_id, H5P_REFERENCE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if(!is_fapl && !is_lapl && !is_rapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not an access property list");

    /* Check if the independent metadata writes property exists on the property list */
    if((prop_exists = H5Pexist(accpl_id, H5_DAOS_IND_MD_IO_PROP_NAME)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for independent metadata I/O property");

    if(prop_exists) {
        /* Get the property */
        if(H5Pget(accpl_id, H5_DAOS_IND_MD_IO_PROP_NAME, is_independent) < 0)
            D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get independent metadata I/O property");
    } /* end if */
    else {
        /* Simply return FALSE as not all metadata I/O
         * operations are independent by default. */
        *is_independent = FALSE;
    } /* end else */

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_get_all_ind_metadata_ops() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_str_prop_delete
 *
 * Purpose:     Property list callback for deleting a string property.
 *              Frees the string.
 *
 * Return:      SUCCEED (never fails)
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_str_prop_delete(hid_t H5VL_DAOS_UNUSED prop_id,
    const char H5VL_DAOS_UNUSED *name, size_t H5VL_DAOS_UNUSED size,
    void *_value)
{
    char **value = (char **)_value;

    if(*value)
        free(*value);

    return SUCCEED;
} /* end H5_daos_str_prop_delete() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_str_prop_copy
 *
 * Purpose:     Property list callback for copying a string property.
 *              Duplicates the string.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_str_prop_copy(const char H5VL_DAOS_UNUSED *name,
    size_t H5VL_DAOS_UNUSED size, void *_value)
{
    char **value = (char **)_value;
    herr_t ret_value = SUCCEED;

    if(*value)
        if(NULL == (*value = strdup(*value)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't copy string property");

done:
    D_FUNC_LEAVE;
} /* end H5_daos_str_prop_copy() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_str_prop_compare
 *
 * Purpose:     Property list callback for comparing string properties.
 *              Compares the strings using strcmp().
 *
 * Return:      SUCCEED (never fails)
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_str_prop_compare(const void *_value1, const void *_value2,
    size_t H5VL_DAOS_UNUSED size)
{
    char * const *value1 = (char * const *)_value1;
    char * const *value2 = (char * const *)_value2;
    int ret_value;

    if(*value1) {
        if(*value2)
            ret_value = strcmp(*value1, *value2);
        else
            ret_value = 1;
    } /* end if */
    else {
        if(*value2)
            ret_value = -1;
        else
            ret_value = 0;
    } /* end else */

    return ret_value;
} /* end H5_daos_str_prop_compare() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_str_prop_close
 *
 * Purpose:     Property list callback for deleting a string property.
 *              Frees the string.
 *
 * Return:      SUCCEED (never fails)
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_str_prop_close(const char H5VL_DAOS_UNUSED *name,
    size_t H5VL_DAOS_UNUSED size, void *_value)
{
    char **value = (char **)_value;

    if(*value)
        free(*value);

    return SUCCEED;
} /* end H5_daos_str_prop_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_bool_prop_compare
 *
 * Purpose:     Property list callback for comparing boolean properties.
 *              Compares the boolean values directly.
 *
 * Return:      SUCCEED (never fails)
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_bool_prop_compare(const void *_value1, const void *_value2,
    size_t H5VL_DAOS_UNUSED size)
{
    const hbool_t *bool1 = (const hbool_t *)_value1;
    const hbool_t *bool2 = (const hbool_t *)_value2;

    return *bool1 == *bool2;
} /* end H5_daos_bool_prop_compare() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_snap_create
 *
 * Purpose:     Creates a snapshot and returns the snapshot ID.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
#ifdef DSINC
herr_t
H5daos_snap_create(hid_t loc_id, H5_daos_snap_id_t *snap_id)
{
    H5_daos_item_t *item;
    H5_daos_file_t *file;
    H5VL_object_t     *obj = NULL;    /* object token of loc_id */
    herr_t          ret_value = SUCCEED;

    if(!snap_id)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "snapshot ID pointer is NULL");

    /* get the location object */
    if(NULL == (obj = (H5VL_object_t *)H5I_object(loc_id)))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid location identifier");

    /* Make sure object's VOL is this one */
    if(obj->driver->id != H5_DAOS_g)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "location does not use DAOS VOL connector");

    /* Get file object */
    if(NULL == (item = H5VLobject(loc_id)))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a VOL object");

    file = item->file;

    /* Check for write access */
    if(!(file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file");

    /* Tell the file to save a snapshot next time it is flushed (committed) */
    file->snap_epoch = (int)TRUE;

    /* Return epoch in snap_id */
    *snap_id = (uint64_t)file->epoch;

done:
    D_FUNC_LEAVE_API;
} /* end H5daos_snap_create() */
#endif


/*-------------------------------------------------------------------------
 * Function:    H5Pset_daos_snap_open
 *
 * XXX: text to be changed
 * Purpose:     Modify the file access property list to use the DAOS VOL
 *              connector defined in this source file.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
#ifdef DV_HAVE_SNAP_OPEN_ID
herr_t
H5Pset_daos_snap_open(hid_t fapl_id, H5_daos_snap_id_t snap_id)
{
    htri_t is_fapl;
    herr_t ret_value = SUCCEED;

    if(fapl_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list");

    if((is_fapl = H5Pisa_class(fapl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class");
    if(!is_fapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list");

    /* Set the property */
    if(H5Pset(fapl_id, H5_DAOS_SNAP_OPEN_ID, &snap_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "can't set property value for snap id");

done:
    D_FUNC_LEAVE_API;
} /* end H5Pset_daos_snap_open() */
#endif


/*-------------------------------------------------------------------------
 * Function:    H5_daos_init
 *
 * Purpose:     Initialize this VOL connector by registering the connector
 *              with the library.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_init(hid_t H5VL_DAOS_UNUSED vipl_id)
{
#ifdef DV_HAVE_SNAP_OPEN_ID
    H5_daos_snap_id_t snap_id_default;
#endif
    char *auto_chunk_str = NULL;
    int ret;
    herr_t ret_value = SUCCEED;            /* Return value */

    if(H5_daos_initialized_g)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "attempting to initialize connector twice");

    if((dv_err_stack_g = H5Ecreate_stack()) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create HDF5 error stack");

    /* Register the connector with HDF5's error reporting API */
    if((dv_err_class_g = H5Eregister_class(DAOS_VOL_ERR_CLS_NAME, DAOS_VOL_ERR_LIB_NAME, DAOS_VOL_ERR_VER)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register error class with HDF5 error API");

    /* Register major error code for failures in object interface */
    if((dv_obj_err_maj_g = H5Ecreate_msg(dv_err_class_g, H5E_MAJOR, "Object interface")) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register error message for object interface");

    /* Register major error code for failures in asynchronous interface */
    if((dv_async_err_g = H5Ecreate_msg(dv_err_class_g, H5E_MAJOR, "Asynchronous interface")) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register error message for asynchronous interface");

#ifdef DV_HAVE_SNAP_OPEN_ID
    /* Register the DAOS SNAP_OPEN_ID property with HDF5 */
    snap_id_default = H5_DAOS_SNAP_ID_INVAL;
    if(H5Pregister2(H5P_FILE_ACCESS, H5_DAOS_SNAP_OPEN_ID, sizeof(H5_daos_snap_id_t), (H5_daos_snap_id_t *) &snap_id_default,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "unable to register DAOS SNAP_OPEN_ID property");
#endif

    /* Initialize daos */
    if((0 != (ret = daos_init())) && (ret != -DER_ALREADY))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "DAOS failed to initialize: %s", H5_daos_err_to_string(ret));

#ifdef DV_TRACK_MEM_USAGE
    /* Initialize allocated memory counter */
    daos_vol_curr_alloc_bytes = 0;
#endif

    /* Set pool globals if they were not already set */
    if(!H5_daos_pool_globals_set_g) {
        uuid_t puuid;

        uuid_clear(puuid);
        if(H5_daos_set_pool_globals(puuid, NULL, NULL) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't set pool globals");
    } /* end if */
    assert(H5_daos_pool_globals_set_g);

    /* Determine if bypassing of the DUNS has been requested */
    if(NULL != getenv("H5_DAOS_BYPASS_DUNS"))
        H5_daos_bypass_duns_g = TRUE;

    /* Determine automatic chunking target size */
    if(NULL != (auto_chunk_str = getenv("H5_DAOS_CHUNK_TARGET_SIZE"))) {
        long long chunk_target_size_ll;

        if((chunk_target_size_ll = strtoll(auto_chunk_str, NULL, 10)) <= 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "failed to parse automatic chunking target size from environment or invalid value (H5_DAOS_CHUNK_TARGET_SIZE)");
        H5_daos_chunk_target_size_g = (uint64_t)chunk_target_size_ll;
    } /* end if */

    /* Initialized */
    H5_daos_initialized_g = TRUE;

done:
    if(ret_value < 0) {
        H5daos_term();
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_init() */


/*---------------------------------------------------------------------------
 * Function:    H5_daos_term
 *
 * Purpose:     Shut down the DAOS VOL
 *
 * Returns:     Non-negative on success/Negative on failure
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5_daos_term(void)
{
    herr_t ret_value = SUCCEED;

    /**
     * H5_DAOS_g is only set if the connector is manually initialized,
     * therefore we must check for proper DAOS initialization.
     */
    if(!H5_daos_initialized_g)
        D_GOTO_DONE(ret_value);

    /* Terminate DAOS */
    if(daos_fini() < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "DAOS failed to terminate");

#ifdef DV_HAVE_SNAP_OPEN_ID
    /* Unregister the DAOS SNAP_OPEN_ID property from HDF5 */
    if(H5Punregister(H5P_FILE_ACCESS, H5_DAOS_SNAP_OPEN_ID) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't unregister DAOS SNAP_OPEN_ID property");
#endif

    /* "Forget" connector id.  This should normally be called by the library
     * when it is closing the id, so no need to close it here. */
    H5_DAOS_g = H5I_INVALID_HID;

    /* No longer initialized */
    H5_daos_initialized_g = FALSE;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_term() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_set_pool_globals
 *
 * Purpose:     Sets global variables that are used when connecting to a
 *              DAOS pool.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_set_pool_globals(uuid_t pool_uuid, const char *pool_grp, const char *pool_svcl)
{
    char *pool_uuid_env = getenv("DAOS_POOL");
    char *pool_grp_env = getenv("DAOS_GROUP");
    char *pool_svcl_env = getenv("DAOS_SVCL");
    d_rank_list_t *svcl = NULL;
    herr_t ret_value = SUCCEED;

    if(pool_grp && (strlen(pool_grp) > H5_DAOS_MAX_GRP_NAME))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "service group name is too long");

    /* Set UUID of DAOS pool to be used */
    memset(H5_daos_pool_uuid_g, 0, sizeof(H5_daos_pool_uuid_g));
    if(pool_uuid_env) {
        if(uuid_parse(pool_uuid_env, H5_daos_pool_uuid_g) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't parse UUID from DAOS_POOL environment variable");
    }
    else if(!uuid_is_null(pool_uuid))
        uuid_copy(H5_daos_pool_uuid_g, pool_uuid);

    /* Set name of DAOS pool group to be used */
    memset(H5_daos_pool_grp_g, '\0', sizeof(H5_daos_pool_grp_g));
    if(pool_grp_env)
        strncpy(H5_daos_pool_grp_g, pool_grp_env, sizeof(H5_daos_pool_grp_g) - 1);
    else
        strncpy(H5_daos_pool_grp_g, pool_grp ? pool_grp : DAOS_DEFAULT_GROUP_ID, sizeof(H5_daos_pool_grp_g) - 1);

    /* Setup pool service replica rank list */
    memset(&H5_daos_pool_svcl_g, 0, sizeof(H5_daos_pool_svcl_g));
    memset(H5_daos_pool_ranks_g, 0, sizeof(H5_daos_pool_ranks_g));
    H5_daos_pool_svcl_g.rl_ranks = H5_daos_pool_ranks_g;
    if(pool_svcl || pool_svcl_env) {
        uint32_t i;

        /* Parse rank list */
        if(NULL == (svcl = daos_rank_list_parse(pool_svcl_env ? pool_svcl_env : pool_svcl, ":")))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "failed to parse service rank list");
        if(svcl->rl_nr == 0 || svcl->rl_nr > H5_DAOS_MAX_SVC_REPLICAS)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "not a valid service list");
        H5_daos_pool_svcl_g.rl_nr = svcl->rl_nr;
        for(i = 0; i < svcl->rl_nr; i++)
            H5_daos_pool_ranks_g[i] = svcl->rl_ranks[i];
    }
    else
        H5_daos_pool_svcl_g.rl_nr = (uint32_t)H5_daos_pool_default_svc_nreplicas_g;

    H5_daos_pool_globals_set_g = TRUE;

done:
    if(svcl)
        daos_rank_list_free(svcl);

    D_FUNC_LEAVE;
} /* end H5_daos_set_pool_globals() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_create
 *
 * Purpose:     Create a pool using default values. This call is collective
 *              across `comm`.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_pool_create(uuid_t uuid, const char **pool_grp, d_rank_list_t **svcl,
    MPI_Comm comm)
{
    unsigned int mode = H5_daos_pool_default_mode_g;
    unsigned int uid = geteuid();
    unsigned int gid = getegid();
    d_rank_list_t *targets = NULL;
    const char *dev = "pmem";
    daos_size_t  scm_size = H5_daos_pool_default_scm_size_g;
    daos_size_t  nvme_size = H5_daos_pool_default_nvme_size_g;
    int comm_size;
    int rank;
    int ret;
    herr_t ret_value = SUCCEED; /* Return value */

    if(MPI_SUCCESS != MPI_Comm_size(comm, &comm_size))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't retrieve size of MPI communicator");
    if(MPI_SUCCESS != MPI_Comm_rank(comm, &rank))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't retrieve rank in MPI communicator");

    /* Create a pool using default values */
    if((rank == 0) && (0 != (ret = daos_pool_create(mode, uid, gid, H5_daos_pool_grp_g,
            targets, dev, scm_size, nvme_size, NULL, &H5_daos_pool_svcl_g,
            H5_daos_pool_uuid_g, NULL /* event */)))) {
        /* Make sure to participate in following broadcast with NULL UUID */
        uuid_clear(H5_daos_pool_uuid_g);
        D_DONE_ERROR(H5E_VOL, H5E_CANTCREATE, FAIL, "can't create pool: %s", H5_daos_err_to_string(ret));
    }

    /* Broadcast UUID and replica service rank list of
     * newly-created pool to other processes if necessary.
     */
    if(comm_size > 1) {
        if(H5_daos_pool_create_bcast(H5_daos_pool_uuid_g,
                &H5_daos_pool_svcl_g, comm, rank) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't broadcast pool connection info");

        if((rank != 0) && (uuid_is_null(H5_daos_pool_uuid_g)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTCREATE, FAIL, "lead process failed to create pool");
    }

    memcpy(uuid, H5_daos_pool_uuid_g, sizeof(uuid_t));
    if(pool_grp) *pool_grp = H5_daos_pool_grp_g;
    if(svcl) *svcl = &H5_daos_pool_svcl_g;

done:
    D_FUNC_LEAVE;
}


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_create_bcast
 *
 * Purpose:     Broadcasts pool connection info, such as the pool UUID and
 *              pool replica service rank list, to other ranks from rank 0
 *              after creation of a DAOS pool.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_pool_create_bcast(uuid_t pool_uuid, d_rank_list_t *pool_svcl,
    MPI_Comm comm, int rank)
{
    H5_daos_pool_create_info pool_create_info;
    MPI_Datatype pci_struct_type = MPI_DATATYPE_NULL;
    MPI_Datatype struct_types[2];
    MPI_Aint displacements[2];
    int blocklens[2];
    herr_t ret_value = SUCCEED;

    assert(pool_svcl);

    memset(&pool_create_info, 0, sizeof(H5_daos_pool_create_info));

    if(rank == 0) {
        uuid_copy(pool_create_info.pool_uuid, pool_uuid);
        pool_create_info.svcl = *pool_svcl;
    }

    /* Create MPI struct type to broadcast pool creation info */
    blocklens[0] = 16;
    blocklens[1] = 1;
    displacements[0] = offsetof(H5_daos_pool_create_info, pool_uuid);
    displacements[1] = offsetof(H5_daos_pool_create_info, svcl.rl_nr);
    struct_types[0] = MPI_CHAR;
    struct_types[1] = MPI_UINT32_T;
    if(MPI_SUCCESS != MPI_Type_create_struct(2, blocklens, displacements,
            struct_types, &pci_struct_type))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't create MPI struct type");

    if(MPI_SUCCESS != MPI_Type_commit(&pci_struct_type))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't commit MPI struct type");

    /* Broadcast pool creation info */
    if(MPI_SUCCESS != MPI_Bcast(&pool_create_info, 1, pci_struct_type, 0, comm))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't broadcast pool creation info");

    /* Set globals related to pool creation on non-zero ranks */
    if(rank != 0) {
        uuid_copy(pool_uuid, pool_create_info.pool_uuid);
        pool_svcl->rl_nr = pool_create_info.svcl.rl_nr;
    }

    /* Broadcast pool replica service rank list */
    if(MPI_SUCCESS != MPI_Bcast(pool_svcl->rl_ranks, (int)pool_svcl->rl_nr,
            MPI_UINT32_T, 0, comm))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't broadcast pool replica service rank list");

done:
    if(MPI_DATATYPE_NULL != pci_struct_type)
        MPI_Type_free(&pci_struct_type);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_create_bcast() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_connect
 *
 * Purpose:     Creates an asynchronous task for connecting to the
 *              specified pool.
 *
 *              DSINC - This routine should eventually be modified to serve
 *                      pool handles from a cache of open handles so that
 *                      we don't re-connect to pools which are already
 *                      connected to when doing multiple file creates/opens
 *                      within the same pool.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_pool_connect(uuid_t *pool_uuid, char *pool_grp, d_rank_list_t *svcl,
    unsigned int flags, daos_handle_t *poh_out, daos_pool_info_t *pool_info_out,
    tse_sched_t *sched, H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_pool_connect_ud_t *connect_udata = NULL;
    tse_task_t *connect_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(pool_uuid);
    assert(pool_grp);
    assert(svcl);
    assert(poh_out);
    assert(sched);
    assert(req);
    assert(first_task);
    assert(dep_task);

    if(NULL == (connect_udata = (H5_daos_pool_connect_ud_t *)DV_malloc(sizeof(H5_daos_pool_connect_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate user data struct for pool connect task");
    connect_udata->req = req;
    connect_udata->puuid = pool_uuid;
    connect_udata->poh = poh_out;
    connect_udata->grp = pool_grp;
    connect_udata->svc = svcl;
    connect_udata->flags = flags;
    connect_udata->info = pool_info_out;
    connect_udata->free_rank_list = FALSE;

    /* Create task for pool connect */
    if(0 != (ret = daos_task_create(DAOS_OPC_POOL_CONNECT, sched,
            *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &connect_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to connect to DAOS pool: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for DAOS pool connect task */
    if(0 != (ret = tse_task_register_cbs(connect_task, H5_daos_pool_connect_prep_cb, NULL, 0, H5_daos_pool_connect_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register callbacks for DAOS pool connect task: %s", H5_daos_err_to_string(ret));

    /* Set private data for pool connect task */
    (void)tse_task_set_priv(connect_task, connect_udata);

    /* Schedule DAOS pool connect task (or save it to be scheduled later) and
     * give it a reference to req */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(connect_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to connect to DAOS pool: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = connect_task;
    req->rc++;

    /* Relinquish control of the pool connect udata to the
     * task's function body */
    connect_udata = NULL;

    *dep_task = connect_task;

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        connect_udata = DV_free(connect_udata);
    } /* end if */

    assert(!connect_udata);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_connect() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_connect_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous daos_pool_connect.
 *              Currently checks for errors from previous tasks then sets
 *              arguments for daos_pool_connect.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_pool_connect_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_pool_connect_ud_t *udata;
    daos_pool_connect_t *connect_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for pool connect task");

    assert(udata->req);
    assert(udata->puuid);

    /* Handle errors */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    if(uuid_is_null(*udata->puuid))
        D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "pool UUID is invalid");

    /* Set daos_pool_connect task args */
    if(NULL == (connect_args = daos_task_get_args(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for pool connect task");
    connect_args->poh = udata->poh;
    connect_args->grp = udata->grp;
    connect_args->svc = udata->svc;
    connect_args->flags = udata->flags;
    connect_args->info = udata->info;
    uuid_copy(connect_args->uuid, *udata->puuid);

done:
    if(ret_value < 0)
        tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_connect_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_connect_comp_cb
 *
 * Purpose:     Completion callback for asynchronous daos_pool_connect.
 *              Currently checks for a failed task then frees private data.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_pool_connect_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_pool_connect_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for DAOS pool connect task");

    assert(udata->req);

    /* Handle errors in daos_pool_connect task.  Only record error in udata->req_status
     * if it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = task->dt_result;
        udata->req->failed_task = "DAOS pool connect";
    } /* end if */
    else if(task->dt_result == 0) {
        /* After connecting to a pool, check if the file object's container_poh
         * field has been set yet. If not, make sure it gets updated with the
         * handle of the pool that we just connected to. This will most often
         * happen during file opens, where the file object's container_poh
         * field is initially invalid.
         */
        if(daos_handle_is_inval(udata->req->file->container_poh))
            udata->req->file->container_poh = *udata->poh;
    } /* end else */

done:
    /* Free private data if we haven't released ownership */
    if(udata) {
        if(udata->free_rank_list && udata->svc)
            daos_rank_list_free(udata->svc);

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except
         * for H5_daos_req_free_int, which updates req->status if it sees an
         * error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->req->status = ret_value;
            udata->req->failed_task = "DAOS pool connect completion callback";
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free private data */
        DV_free(udata);
    } /* end if */
    else
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_connect_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_disconnect
 *
 * Purpose:     Creates an asynchronous task for disconnecting from the
 *              specified pool.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_pool_disconnect(daos_handle_t *poh, tse_sched_t *sched,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_pool_disconnect_ud_t *disconnect_udata = NULL;
    tse_task_t *disconnect_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(poh);
    assert(sched);
    assert(req);
    assert(first_task);
    assert(dep_task);

    if(NULL == (disconnect_udata = (H5_daos_pool_disconnect_ud_t *)DV_malloc(sizeof(H5_daos_pool_disconnect_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate user data struct for pool disconnect task");
    disconnect_udata->req = req;
    disconnect_udata->poh = poh;

    /* Create task for pool disconnect */
    if(0 != (ret = daos_task_create(DAOS_OPC_POOL_DISCONNECT, sched,
            *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &disconnect_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to disconnect from DAOS pool: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for DAOS pool disconnect task */
    if(0 != (ret = tse_task_register_cbs(disconnect_task, H5_daos_pool_disconnect_prep_cb, NULL, 0, H5_daos_pool_disconnect_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register callbacks for DAOS pool disconnect task: %s", H5_daos_err_to_string(ret));

    /* Set private data for pool disconnect task */
    (void)tse_task_set_priv(disconnect_task, disconnect_udata);

    /* Schedule DAOS pool disconnect task (or save it to be scheduled later) and
     * give it a reference to req */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(disconnect_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to disconnect from DAOS pool: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = disconnect_task;
    req->rc++;

    /* Relinquish control of the pool disconnect udata to the
     * task's function body */
    disconnect_udata = NULL;

    *dep_task = disconnect_task;

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        disconnect_udata = DV_free(disconnect_udata);
    } /* end if */

    assert(!disconnect_udata);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_disconnect() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_disconnect_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous daos_pool_disconnect.
 *              Currently checks for errors from previous tasks then sets
 *              arguments for daos_pool_disconnect.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_pool_disconnect_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_pool_disconnect_ud_t *udata;
    daos_pool_disconnect_t *disconnect_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for pool disconnect task");

    assert(udata->req);
    assert(udata->poh);

    /* Handle errors */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    if(daos_handle_is_inval(*udata->poh))
        D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "pool handle is invalid");

    /* Set daos_pool_disconnect task args */
    if(NULL == (disconnect_args = daos_task_get_args(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for pool disconnect task");
    disconnect_args->poh = *udata->poh;

done:
    if(ret_value < 0)
        tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_disconnect_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_disconnect_comp_cb
 *
 * Purpose:     Completion callback for asynchronous daos_pool_disconnect.
 *              Currently checks for a failed task then frees private data.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_pool_disconnect_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_pool_disconnect_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for DAOS pool disconnect task");

    assert(udata->req);

    /* Handle errors in daos_pool_disconnect task.  Only record error in udata->req_status
     * if it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = task->dt_result;
        udata->req->failed_task = "DAOS pool disconnect";
    } /* end if */

done:
    /* Free private data if we haven't released ownership */
    if(udata) {
        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except
         * for H5_daos_req_free_int, which updates req->status if it sees an
         * error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->req->status = ret_value;
            udata->req->failed_task = "DAOS pool disconnect completion callback";
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free private data */
        DV_free(udata);
    } /* end if */
    else
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_disconnect_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_query
 *
 * Purpose:     Creates an asynchronous task for querying information from
 *              a DAOS pool.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_pool_query(daos_handle_t *poh, daos_pool_info_t *pool_info,
    d_rank_list_t *tgts, daos_prop_t *prop, tse_sched_t *sched,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_pool_query_ud_t *query_ud = NULL;
    tse_task_t *query_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(poh);
    assert(sched);
    assert(req);
    assert(first_task);
    assert(dep_task);

    if(NULL == (query_ud = (H5_daos_pool_query_ud_t *)DV_malloc(sizeof(H5_daos_pool_query_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate user data struct for pool query task");
    query_ud->generic_ud.req = req;
    query_ud->generic_ud.task_name = "pool query";
    query_ud->poh = poh;
    query_ud->pool_info = pool_info;
    query_ud->tgts = tgts;
    query_ud->prop = prop;

    /* Create task for pool query operation */
    if(0 != (ret = daos_task_create(DAOS_OPC_POOL_QUERY, sched,
            *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &query_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to query pool: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for pool query */
    if(0 != (ret = tse_task_register_cbs(query_task, H5_daos_pool_query_prep_cb, NULL, 0, H5_daos_generic_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register callbacks for task to query pool: %s", H5_daos_err_to_string(ret));

    /* Set private data for pool query */
    (void)tse_task_set_priv(query_task, query_ud);

    /* Schedule pool query task (or save it to be scheduled later)
     * and give it a reference to req */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(query_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to query pool: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = query_task;
    req->rc++;
    query_ud = NULL;
    *dep_task = query_task;

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        query_ud = DV_free(query_ud);
    } /* end if */

    /* Make sure we cleaned up */
    assert(!query_ud);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_query() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_pool_query_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous daos_pool_query.
 *              Currently checks for errors from previous tasks then sets
 *              arguments for daos_pool_query.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_pool_query_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_pool_query_ud_t *udata;
    daos_pool_query_t *query_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for pool query task");

    assert(udata->generic_ud.req);
    assert(udata->poh);

    /* Handle errors */
    if(udata->generic_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->generic_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    if(daos_handle_is_inval(*udata->poh))
        D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, -H5_DAOS_BAD_VALUE, "pool handle is invalid");

    /* Set query task's arguments */
    if(NULL == (query_args = daos_task_get_args(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for pool query task");
    query_args->poh = *udata->poh;
    query_args->info = udata->pool_info;
    query_args->tgts = udata->tgts;
    query_args->prop = udata->prop;

done:
    if(ret_value < 0)
        tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_pool_query_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_fapl_copy
 *
 * Purpose:     Copies the DAOS-specific file access properties.
 *
 * Return:      Success:        Ptr to a new property list
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_fapl_copy(const void *_old_fa)
{
    const H5_daos_fapl_t *old_fa = (const H5_daos_fapl_t*)_old_fa;
    H5_daos_fapl_t       *new_fa = NULL;
    void                 *ret_value = NULL;

    if(!_old_fa)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "invalid fapl");

    if(NULL == (new_fa = (H5_daos_fapl_t *)DV_malloc(sizeof(H5_daos_fapl_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "memory allocation failed");

    /* Copy the general information */
    memcpy(new_fa, old_fa, sizeof(H5_daos_fapl_t));

    /* Clear allocated fields, so they aren't freed if something goes wrong.  No
     * need to clear info since it is only freed if comm is not null. */
    new_fa->comm = MPI_COMM_NULL;

    /* Duplicate communicator and Info object. */
    if(FAIL == H5_daos_comm_info_dup(old_fa->comm, old_fa->info, &new_fa->comm, &new_fa->info))
        D_GOTO_ERROR(H5E_INTERNAL, H5E_CANTCOPY, NULL, "failed to duplicate MPI communicator and info");
    new_fa->free_comm_info = TRUE;

    ret_value = new_fa;

done:
    if(NULL == ret_value) {
        /* cleanup */
        if(new_fa && H5_daos_fapl_free(new_fa) < 0)
            D_DONE_ERROR(H5E_PLIST, H5E_CANTFREE, NULL, "can't free fapl");
    } /* end if */

    D_FUNC_LEAVE_API;
} /* end H5_daos_fapl_copy() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_fapl_free
 *
 * Purpose:     Frees the DAOS-specific file access properties.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_fapl_free(void *_fa)
{
    H5_daos_fapl_t *fa = (H5_daos_fapl_t*) _fa;
    herr_t          ret_value = SUCCEED;

    if(!_fa)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid fapl");

    /* Free the internal communicator and INFO object */
    if(fa->free_comm_info && fa->comm != MPI_COMM_NULL)
        if(H5_daos_comm_info_free(&fa->comm, &fa->info) < 0)
            D_GOTO_ERROR(H5E_INTERNAL, H5E_CANTFREE, FAIL, "failed to free copy of MPI communicator and info");

    /* free the struct */
    DV_free(fa);

done:
    D_FUNC_LEAVE_API;
} /* end H5_daos_fapl_free() */


/*---------------------------------------------------------------------------
 * Function:    H5_daos_get_conn_cls
 *
 * Purpose:     Query the connector class.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5_daos_get_conn_cls(void *item, H5VL_get_conn_lvl_t H5VL_DAOS_UNUSED lvl,
    const H5VL_class_t **conn_cls)
{
    herr_t          ret_value = SUCCEED;

    if(!item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "item parameter not supplied");
    if(!conn_cls)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "conn_cls parameter not supplied");

    H5_DAOS_MAKE_ASYNC_PROGRESS(((H5_daos_item_t *)item)->file->sched, FAIL);

    /* Retrieve the DAOS VOL connector class */
    *conn_cls = &H5_daos_g;

done:
    D_FUNC_LEAVE_API;
} /* end H5_daos_get_conn_cls() */


/*---------------------------------------------------------------------------
 * Function:    H5_daos_opt_query
 *
 * Purpose:     Query if an optional operation is supported by this connector
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5_daos_opt_query(void *item, H5VL_subclass_t H5VL_DAOS_UNUSED cls,
    int H5VL_DAOS_UNUSED opt_type, hbool_t *supported)
{
    herr_t          ret_value = SUCCEED;

    if(!item)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "\"item\" parameter not supplied");
    if(!supported)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "\"supported\" parameter not supplied");

    H5_DAOS_MAKE_ASYNC_PROGRESS(((H5_daos_item_t *)item)->file->sched, FAIL);

    /* This VOL connector currently supports no optional operations queried by
     * this function */
    *supported = FALSE;

done:
    D_FUNC_LEAVE_API;
} /* end H5_daos_opt_query() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_optional
 *
 * Purpose:     Optional VOL callbacks.  Thin switchboard to translate map
 *              object calls to a format analogous to other VOL object
 *              callbacks.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5_daos_optional(void *item, int op_type, hid_t dxpl_id,
    void **req, va_list arguments)
{
    herr_t          ret_value = SUCCEED;

    /* Check operation type */
    switch(op_type) {
        /* H5Mcreate/create_anon */
        case H5VL_MAP_CREATE:
        {
            const H5VL_loc_params_t *loc_params = va_arg(arguments, const H5VL_loc_params_t *);
            const char *name = va_arg(arguments, const char *);
            hid_t lcpl_id = va_arg(arguments, hid_t);
            hid_t ktype_id = va_arg(arguments, hid_t);
            hid_t vtype_id = va_arg(arguments, hid_t);
            hid_t mcpl_id = va_arg(arguments, hid_t);
            hid_t mapl_id = va_arg(arguments, hid_t);
            void **map = va_arg(arguments, void **);

            /* Check map argument.  All other arguments will be checked by
             * H5_daos_map_create. */
            if(!map)
                D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "map object output parameter is NULL");

            /* Pass the call */
            if(NULL == (*map = H5_daos_map_create(item, loc_params, name, lcpl_id, ktype_id, vtype_id,
                    mcpl_id, mapl_id, dxpl_id, req)))
                D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, FAIL, "can't create map object");

            break;
        } /* end block */

        /* H5Mopen */
        case H5VL_MAP_OPEN:
        {
            const H5VL_loc_params_t *loc_params = va_arg(arguments, const H5VL_loc_params_t *);
            const char *name = va_arg(arguments, const char *);
            hid_t mapl_id = va_arg(arguments, hid_t);
            void **map = va_arg(arguments, void **);

            /* Check map argument.  All other arguments will be checked by
             * H5_daos_map_open. */
            if(!map)
                D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "map object output parameter is NULL");

            /* Pass the call */
            if(NULL == (*map = H5_daos_map_open(item, loc_params, name, mapl_id, dxpl_id, req)))
                D_GOTO_ERROR(H5E_MAP, H5E_CANTOPENOBJ, FAIL, "can't open map object");

            break;
        } /* end block */

        /* H5Mget */
        case H5VL_MAP_GET_VAL:
        {
            hid_t key_mem_type_id = va_arg(arguments, hid_t);
            const void *key = va_arg(arguments, const void *);
            hid_t val_mem_type_id = va_arg(arguments, hid_t);
            void *value = va_arg(arguments, void *);

            /* All arguments will be checked by H5_daos_map_get_val. */

            /* Pass the call */
            if((ret_value = H5_daos_map_get_val(item, key_mem_type_id, key, val_mem_type_id, value, dxpl_id, req)) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_READERROR, ret_value, "can't get value");

            break;
        } /* end block */

        /* H5Mexists */
        case H5VL_MAP_EXISTS:
        {
            hid_t key_mem_type_id = va_arg(arguments, hid_t);
            const void *key = va_arg(arguments, const void *);
            hbool_t *exists = va_arg(arguments, hbool_t *);

            /* All arguments will be checked by H5_daos_map_exists. */

            /* Pass the call */
            if((ret_value = H5_daos_map_exists(item, key_mem_type_id, key, exists, dxpl_id, req)) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_READERROR, ret_value, "can't check if value exists");

            break;
        } /* end block */

        /* H5Mput */
        case H5VL_MAP_PUT:
        {
            hid_t key_mem_type_id = va_arg(arguments, hid_t);
            const void *key = va_arg(arguments, const void *);
            hid_t val_mem_type_id = va_arg(arguments, hid_t);
            const void *value = va_arg(arguments, const void *);

            /* All arguments will be checked by H5_daos_map_put. */

            /* Pass the call */
            if((ret_value = H5_daos_map_put(item, key_mem_type_id, key, val_mem_type_id, value, dxpl_id, req)) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_WRITEERROR, ret_value, "can't put value");

            break;
        } /* end block */

        /* Operations that get misc info from the map */
        case H5VL_MAP_GET:
        {
            H5VL_map_get_t get_type = va_arg(arguments, H5VL_map_get_t);

            /* All arguments will be checked by H5_daos_map_get. */

            /* Pass the call */
            if((ret_value = H5_daos_map_get(item, get_type, dxpl_id, req, arguments)) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, ret_value, "can't perform map get operation");

            break;
        } /* end block */

        /* Specific operations (H5Miterate and H5Mdelete) */
        case H5VL_MAP_SPECIFIC:
        {
            const H5VL_loc_params_t *loc_params = va_arg(arguments, const H5VL_loc_params_t *);
            H5VL_map_specific_t specific_type = va_arg(arguments, H5VL_map_specific_t);

            /* All arguments will be checked by H5_daos_map_specific. */

            /* Pass the call */
            if((ret_value = H5_daos_map_specific(item, loc_params, specific_type, dxpl_id, req, arguments)) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, ret_value, "can't perform specific map operation");

            break;
        } /* end block */

        /* H5Mclose */
        case H5VL_MAP_CLOSE:
        {
            /* Pass the call */
            if((ret_value = H5_daos_map_close(item, dxpl_id, req)) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_CLOSEERROR, ret_value, "can't close map object");

            break;
        } /* end block */

        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "invalid or unsupported optional operation");
    } /* end switch */

done:
    D_FUNC_LEAVE_API;
} /* end H5_daos_optional() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oidx_generate
 *
 * Purpose:     Generates a unique 64 bit object index.  This index will be
 *              used as the lower 64 bits of a DAOS object ID. If
 *              necessary, this routine creates a task to allocate
 *              additional object indices for the given container before
 *              generating the object index that is returned.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_oidx_generate(uint64_t *oidx, H5_daos_file_t *file, hbool_t collective,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_oidx_generate_ud_t *generate_udata = NULL;
    daos_cont_alloc_oids_t *alloc_args;
    tse_task_t *generate_task = NULL;
    uint64_t *next_oidx = collective ? &file->next_oidx_collective : &file->next_oidx;
    uint64_t *max_oidx = collective ? &file->max_oidx_collective : &file->max_oidx;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(file);
    assert(req);
    assert(first_task);
    assert(dep_task);

    /* Allocate more object indices for this process if necessary */
    if((*max_oidx == 0) || (*next_oidx > *max_oidx)) {
        /* Check if this process should allocate object IDs or just wait for the
         * result from the leader process */
        if(!collective || (file->my_rank == 0)) {
            /* Create task to allocate oidxs */
            if(0 != (ret = daos_task_create(DAOS_OPC_CONT_ALLOC_OIDS, &file->sched, *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &generate_task)))
                D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to generate OIDXs: %s", H5_daos_err_to_string(ret));

            /* Set callback functions for container open */
            if(0 != (ret = tse_task_register_cbs(generate_task, H5_daos_generic_prep_cb, NULL, 0, H5_daos_oidx_generate_comp_cb, NULL, 0)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "can't register callbacks for task to generate OIDXs: %s", H5_daos_err_to_string(ret));

            /* Set private data for OIDX generation task */
            if(NULL == (generate_udata = (H5_daos_oidx_generate_ud_t *)DV_malloc(sizeof(H5_daos_oidx_generate_ud_t))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate user data struct for OIDX generation task");
            generate_udata->generic_ud.req = req;
            generate_udata->generic_ud.task_name = "OIDX generation";
            generate_udata->file = file;
            generate_udata->collective = collective;
            generate_udata->oidx_out = oidx;
            generate_udata->next_oidx = next_oidx;
            generate_udata->max_oidx = max_oidx;
            (void)tse_task_set_priv(generate_task, generate_udata);

            /* Set arguments for OIDX generation */
            if(NULL == (alloc_args = daos_task_get_args(generate_task)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTGET, FAIL, "can't get arguments for OIDX generation task");
            alloc_args->coh = file->coh;
            alloc_args->num_oids = H5_DAOS_OIDX_NALLOC;
            alloc_args->oid = next_oidx;

            /* Schedule OIDX generation task (or save it to be scheduled later) and give it
             * a reference to req */
            if(*first_task) {
                if(0 != (ret = tse_task_schedule(generate_task, false)))
                    D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to generate OIDXs: %s", H5_daos_err_to_string(ret));
            }
            else
                *first_task = generate_task;
            req->rc++;
            file->item.rc++;

            /* Relinquish control of the OIDX generation udata to the
             * task's completion callback */
            generate_udata = NULL;

            *dep_task = generate_task;
        } /* end if */

        /* Broadcast next_oidx if there are other processes that need it */
        if(collective && (file->num_procs > 1) && H5_daos_oidx_bcast(file, oidx, req, first_task, dep_task) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTSET, FAIL, "can't broadcast next object index");
    } /* end if */
    else {
        /* Allocate oidx from local allocation */
        H5_DAOS_ALLOCATE_NEXT_OIDX(oidx, next_oidx, max_oidx);
    }

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        generate_udata = DV_free(generate_udata);
    }

    /* Make sure we cleaned up */
    assert(!generate_udata);

    D_FUNC_LEAVE;
} /* end H5_daos_oidx_generate() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oidx_generate_comp_cb
 *
 * Purpose:     Complete callback for the DAOS OIDX generation task. When
 *              H5_daos_oidx_generate is called independently, this
 *              callback is responsible for updating the current process'
 *              file's max_oidx and next_oidx fields and "allocating" the
 *              actually returned next oidx. When H5_daos_oidx_generate is
 *              called collectively, the completion callback for the
 *              ensuing oidx broadcast task will be responsible for these
 *              tasks instead.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_oidx_generate_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_oidx_generate_ud_t *udata;
    uint64_t *next_oidx;
    uint64_t *max_oidx;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for OIDX generation task");

    assert(udata->file);
    assert(!udata->file->closed);

    /* Handle errors in OIDX generation task.  Only record error in udata->req_status if
     * it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->generic_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->generic_ud.req->status = task->dt_result;
        udata->generic_ud.req->failed_task = udata->generic_ud.task_name;
    } /* end if */
    else if(task->dt_result == 0) {
        next_oidx = udata->next_oidx;
        max_oidx = udata->max_oidx;

        /* If H5_daos_oidx_generate was called independently, it is
         * safe to update the file's max and next OIDX fields and
         * allocate the next OIDX. Otherwise, this must be delayed
         * until after the next OIDX value has been broadcasted to
         * the other ranks.
         */
        if(!udata->collective || (udata->generic_ud.req->file->num_procs == 1)) {
            /* Adjust the max and next OIDX values for the file on this process */
            H5_DAOS_ADJUST_MAX_AND_NEXT_OIDX(next_oidx, max_oidx);

            /* Allocate oidx from local allocation */
            H5_DAOS_ALLOCATE_NEXT_OIDX(udata->oidx_out, next_oidx, max_oidx);
        }
    } /* end if */

done:
    if(udata) {
        /* Release our reference on the file */
        H5_daos_file_decref(udata->file);

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->generic_ud.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->generic_ud.req->status = ret_value;
            udata->generic_ud.req->failed_task = udata->generic_ud.task_name;
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->generic_ud.req) < 0)
            D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free private data */
        DV_free(udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_oidx_generate_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oidx_bcast
 *
 * Purpose:     Creates an asynchronous task for broadcasting the next OIDX
 *              value after rank 0 has allocated more from DAOS.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_oidx_bcast(H5_daos_file_t *file, uint64_t *oidx_out,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_oidx_bcast_ud_t *oidx_bcast_udata = NULL;
    tse_task_t *bcast_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(file);
    assert(oidx_out);
    assert(req);
    assert(first_task);
    assert(dep_task);

    /* Set up broadcast user data */
    if(NULL == (oidx_bcast_udata = (H5_daos_oidx_bcast_ud_t *)DV_malloc(sizeof(H5_daos_oidx_bcast_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate buffer for MPI broadcast user data");
    oidx_bcast_udata->bcast_udata.req = req;
    oidx_bcast_udata->bcast_udata.obj = NULL;
    oidx_bcast_udata->bcast_udata.sched = &file->sched;
    oidx_bcast_udata->bcast_udata.bcast_metatask = NULL;
    oidx_bcast_udata->bcast_udata.buffer = oidx_bcast_udata->next_oidx_buf;
    oidx_bcast_udata->bcast_udata.buffer_len = H5_DAOS_ENCODED_UINT64_T_SIZE;
    oidx_bcast_udata->bcast_udata.count = H5_DAOS_ENCODED_UINT64_T_SIZE;
    oidx_bcast_udata->file = file;
    oidx_bcast_udata->oidx_out = oidx_out;
    oidx_bcast_udata->next_oidx = &file->next_oidx_collective;
    oidx_bcast_udata->max_oidx = &file->max_oidx_collective;

    /* Create task for broadcast */
    if(0 != (ret = tse_task_create(H5_daos_mpi_ibcast_task, &file->sched, oidx_bcast_udata, &bcast_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to broadcast next object index: %s", H5_daos_err_to_string(ret));

    /* Register dependency on dep_task if present */
    if(*dep_task && 0 != (ret = tse_task_register_deps(bcast_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create dependencies for next object index broadcast task: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for next object index bcast */
    if(0 != (ret = tse_task_register_cbs(bcast_task, (file->my_rank == 0) ? H5_daos_oidx_bcast_prep_cb : NULL,
            NULL, 0, H5_daos_oidx_bcast_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register callbacks for next object index broadcast: %s", H5_daos_err_to_string(ret));

    /* Schedule OIDX broadcast task (or save it to be scheduled later) and give it
     * a reference to req */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(bcast_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to broadcast next object index: %s", H5_daos_err_to_string(ret));
    }
    else
        *first_task = bcast_task;
    req->rc++;
    file->item.rc++;

    /* Relinquish control of the OIDX broadcast udata to the
     * task's completion callback */
    oidx_bcast_udata = NULL;

    *dep_task = bcast_task;

done:
    /* Cleanup on failure */
    if(oidx_bcast_udata) {
        assert(ret_value < 0);
        oidx_bcast_udata = DV_free(oidx_bcast_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_oidx_bcast() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oidx_bcast_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous OIDX broadcasts.
 *              Currently checks for errors from previous tasks and then
 *              encodes the OIDX value into the broadcast buffer before
 *              sending. Meant only to be called by rank 0.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_oidx_bcast_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_oidx_bcast_ud_t *udata;
    uint8_t *p;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for object index broadcast task");

    assert(udata->bcast_udata.req);
    assert(udata->bcast_udata.buffer);
    assert(udata->next_oidx);
    assert(H5_DAOS_ENCODED_UINT64_T_SIZE == udata->bcast_udata.buffer_len);
    assert(H5_DAOS_ENCODED_UINT64_T_SIZE == udata->bcast_udata.count);

    /* Note that we do not handle errors from a previous task here.
     * The broadcast must still proceed on all ranks even if a
     * previous task has failed.
     */

    p = udata->bcast_udata.buffer;
    UINT64ENCODE(p, *udata->next_oidx);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_oidx_bcast_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oidx_bcast_comp_cb
 *
 * Purpose:     Complete callback for asynchronous OIDX broadcasts.
 *              Currently checks for a failed task, then performs the
 *              following in order:
 *
 *              - decodes the sent OIDX buffer on the ranks that are
 *                receiving it
 *              - adjusts the max OIDX and next OIDX fields in the file on
 *                all ranks
 *              - allocates the next OIDX value on all ranks
 *              - frees private data
 *
 *              Meant to be called by all ranks.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_oidx_bcast_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_oidx_bcast_ud_t *udata;
    uint64_t *next_oidx;
    uint64_t *max_oidx;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for object index broadcast task");

    assert(udata->bcast_udata.req);
    assert(udata->bcast_udata.buffer);
    assert(udata->file);
    assert(udata->oidx_out);
    assert(udata->next_oidx);
    assert(udata->max_oidx);
    assert(H5_DAOS_ENCODED_UINT64_T_SIZE == udata->bcast_udata.buffer_len);
    assert(H5_DAOS_ENCODED_UINT64_T_SIZE == udata->bcast_udata.count);

    /* Handle errors in OIDX broadcast task.  Only record error in
     * udata->req_status if it does not already contain an error (it could
     * contain an error if another task this task is not dependent on also
     * failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->bcast_udata.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->bcast_udata.req->status = task->dt_result;
        udata->bcast_udata.req->failed_task = "MPI_Ibcast next object index";
    } /* end if */
    else if(task->dt_result == 0) {
        next_oidx = udata->next_oidx;
        max_oidx = udata->max_oidx;

        /* Decode sent OIDX on receiving ranks */
        if(udata->bcast_udata.req->file->my_rank != 0) {
            uint8_t *p = udata->bcast_udata.buffer;
            UINT64DECODE(p, *next_oidx);
        }

        /* Adjust the max and next OIDX values for the file on this process */
        H5_DAOS_ADJUST_MAX_AND_NEXT_OIDX(next_oidx, max_oidx);

        /* Allocate oidx from local allocation */
        H5_DAOS_ALLOCATE_NEXT_OIDX(udata->oidx_out, next_oidx, max_oidx);
    } /* end else */

done:
    if(udata) {
        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except
         * for H5_daos_req_free_int, which updates req->status if it sees an
         * error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->bcast_udata.req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->bcast_udata.req->status = ret_value;
            udata->bcast_udata.req->failed_task = "MPI_Ibcast next object index completion callback";
        } /* end if */

        H5_daos_file_decref(udata->file);

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->bcast_udata.req) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free private data */
        DV_free(udata);
    }
    else
        assert(ret_value >= 0 || ret_value == -H5_DAOS_DAOS_GET_ERROR);

    D_FUNC_LEAVE;
} /* end H5_daos_oidx_bcast_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oid_encode
 *
 * Purpose:     Creates a DAOS OID given the object type and a 64 bit
 *              object index.  Note that `file` must have at least the
 *              default_object_class field set, but may be otherwise
 *              uninitialized.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_oid_encode(daos_obj_id_t *oid, uint64_t oidx, H5I_type_t obj_type,
    hid_t crt_plist_id, const char *oclass_prop_name, H5_daos_file_t *file)
{
    daos_oclass_id_t object_class = OC_UNKNOWN;
    daos_ofeat_t object_feats;
    herr_t ret_value = SUCCEED;

    /* Initialize oid.lo to oidx */
    oid->lo = oidx;

    /* Set type bits in the upper 2 bits of the lower 32 of oid.hi (for
     * simplicity so they're in the same location as in the compacted haddr_t
     * form) */
    if(obj_type == H5I_GROUP)
        oid->hi = H5_DAOS_TYPE_GRP;
    else if(obj_type == H5I_DATASET)
        oid->hi = H5_DAOS_TYPE_DSET;
    else if(obj_type == H5I_DATATYPE)
        oid->hi = H5_DAOS_TYPE_DTYPE;
    else {
        assert(obj_type == H5I_MAP);
        oid->hi = H5_DAOS_TYPE_MAP;
    } /* end else */

    /* Set the object feature flags */
    if(H5I_GROUP == obj_type)
        object_feats = DAOS_OF_DKEY_LEXICAL | DAOS_OF_AKEY_LEXICAL;
    else
        object_feats = DAOS_OF_DKEY_HASHED | DAOS_OF_AKEY_LEXICAL;

    /* Check for object class set on crt_plist_id */
    /* Note we do not copy the oclass_str in the property callbacks (there is no
     * "get" callback, so this is more like an H5P_peek, and we do not need to
     * free oclass_str as it points directly into the plist value */
    if(crt_plist_id != H5P_DEFAULT) {
        htri_t prop_exists;

        if((prop_exists = H5Pexist(crt_plist_id, oclass_prop_name)) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't check for object class property");
        if(prop_exists) {
            char *oclass_str = NULL;

            if(H5Pget(crt_plist_id, oclass_prop_name, &oclass_str) < 0)
                D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "can't get object class");
            if(oclass_str && (oclass_str[0] != '\0'))
                if(OC_UNKNOWN == (object_class = (daos_oclass_id_t)daos_oclass_name2id(oclass_str)))
                    D_GOTO_ERROR(H5E_VOL, H5E_CANTGET, FAIL, "unknown object class");
        } /* end if */
    } /* end if */

    /* Check for object class set on file if not set from plist */
    if(object_class == OC_UNKNOWN)
        object_class = file->fapl_cache.default_object_class;

    /* Set the object class by default according to object type if not set from
     * above */
    if(object_class == OC_UNKNOWN)
        object_class = (obj_type == H5I_DATASET) ? OC_SX : OC_S1;

    /* Generate oid */
    H5_daos_obj_generate_id(oid, object_feats, object_class);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_oid_encode() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oid_encode_task
 *
 * Purpose:     Asynchronous task for calling H5_daos_oid_encode.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_oid_encode_task(tse_task_t *task)
{
    H5_daos_oid_encode_ud_t *udata = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for OID encoding task");

    assert(udata->req);
    assert(udata->oid_out);

    /* Check for previous errors */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT)
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT)
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);

    if(H5_daos_oid_encode(udata->oid_out, udata->oidx, udata->obj_type,
            udata->crt_plist_id, udata->oclass_prop_name, udata->req->file) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTENCODE, -H5_DAOS_H5_ENCODE_ERROR, "can't encode object ID");

done:
    /* Free private data if we haven't released ownership */
    if(udata) {
        if(H5P_DEFAULT != udata->crt_plist_id)
            if(H5Idec_ref(udata->crt_plist_id) < 0)
                D_DONE_ERROR(H5E_PLIST, H5E_CANTDEC, -H5_DAOS_H5_CLOSE_ERROR, "can't decrement ref. count on creation plist");

        H5_daos_file_decref(udata->file);

        /* Handle errors in this function */
        /* Do not place any code that can issue errors after this block, except for
         * H5_daos_req_free_int, which updates req->status if it sees an error */
        if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
            udata->req->status = ret_value;
            udata->req->failed_task = "OID encoding task";
        } /* end if */

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free private data */
        DV_free(udata);
    } /* end if */
    else
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);

    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_oid_encode_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oid_generate
 *
 * Purpose:     Generate a DAOS OID given the object type and file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_oid_generate(daos_obj_id_t *oid, H5I_type_t obj_type,
    hid_t crt_plist_id, H5_daos_file_t *file, hbool_t collective,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_oid_encode_ud_t *encode_udata = NULL;
    tse_task_t *dep_task_orig;
    tse_task_t *encode_task = NULL;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(file);
    assert(req);
    assert(first_task);
    assert(dep_task);

    /* Track originally passed in dep task */
    dep_task_orig = *dep_task;

    /* Set up user data for OID encoding */
    if(NULL == (encode_udata = (H5_daos_oid_encode_ud_t *)DV_malloc(sizeof(H5_daos_oid_encode_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate buffer for OID encoding user data");
    encode_udata->req = req;
    encode_udata->oid_out = oid;

    /* Generate oidx */
    if(H5_daos_oidx_generate(&encode_udata->oidx, file, collective, req, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTALLOC, FAIL, "can't generate object index");

    /* If OIDX generation created tasks, the following OID encoding must also
     * create tasks to depend on those tasks. Otherwise, the encoding proceeds
     * synchronously.
     */
    if(dep_task_orig == *dep_task) {
        /* Encode oid */
        if(H5_daos_oid_encode(encode_udata->oid_out, encode_udata->oidx, obj_type,
                crt_plist_id, H5_DAOS_OBJ_CLASS_NAME, file) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTENCODE, FAIL, "can't encode object ID");
    }
    else {
        /* Create asynchronous task for OID encoding */

        encode_udata->file = file;
        encode_udata->obj_type = obj_type;
        encode_udata->crt_plist_id = crt_plist_id;
        encode_udata->oclass_prop_name = H5_DAOS_OBJ_CLASS_NAME;

        /* Create task to encode OID */
        if(0 != (ret = tse_task_create(H5_daos_oid_encode_task, &file->sched, encode_udata, &encode_task)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to encode OID: %s", H5_daos_err_to_string(ret));

        /* Register task dependency */
        if(*dep_task && 0 != (ret = tse_task_register_deps(encode_task, 1, dep_task)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create dependencies for OID encoding task: %s", H5_daos_err_to_string(ret));

        /* Schedule OID encoding task (or save it to be scheduled later) and give it
         * a reference to req */
        if(*first_task) {
            if(0 != (ret = tse_task_schedule(encode_task, false)))
                D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to encode OID: %s", H5_daos_err_to_string(ret));
        }
        else
            *first_task = encode_task;
        req->rc++;
        file->item.rc++;

        /* Relinquish control of the OID encoding udata to the
         * task's completion callback */
        encode_udata = NULL;

        if(H5P_DEFAULT != crt_plist_id)
            if(H5Iinc_ref(crt_plist_id) < 0)
                D_GOTO_ERROR(H5E_PLIST, H5E_CANTINC, FAIL, "can't increment ref. count on creation plist");

        *dep_task = encode_task;
    }

done:
    encode_udata = DV_free(encode_udata);

    D_FUNC_LEAVE;
} /* end H5_daos_oid_generate() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oid_to_token
 *
 * Purpose:     Converts an OID to an object "token".
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_oid_to_token(daos_obj_id_t oid, H5O_token_t *obj_token)
{
    uint8_t *p;
    herr_t ret_value = SUCCEED;

    assert(obj_token);
    H5daos_compile_assert(H5_DAOS_ENCODED_OID_SIZE <= H5O_MAX_TOKEN_SIZE);

    p = (uint8_t *) obj_token;

    UINT64ENCODE(p, oid.lo);
    UINT64ENCODE(p, oid.hi);

    D_FUNC_LEAVE;
} /* end H5_daos_oid_to_token() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_token_to_oid
 *
 * Purpose:     Converts an object "token" to an OID.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_token_to_oid(const H5O_token_t *obj_token, daos_obj_id_t *oid)
{
    const uint8_t *p;
    herr_t ret_value = SUCCEED;

    assert(obj_token);
    assert(oid);
    H5daos_compile_assert(H5_DAOS_ENCODED_OID_SIZE <= H5O_MAX_TOKEN_SIZE);

    p = (const uint8_t *) obj_token;

    UINT64DECODE(p, oid->lo);
    UINT64DECODE(p, oid->hi);

    D_FUNC_LEAVE;
} /* end H5_daos_token_to_oid() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_oid_to_type
 *
 * Purpose:     Retrieve the HDF5 object type from an OID
 *
 * Return:      Success:    Object type
 *              Failure:    H5I_BADID
 *
 *-------------------------------------------------------------------------
 */
H5I_type_t
H5_daos_oid_to_type(daos_obj_id_t oid)
{
    uint64_t type_bits;

    /* Retrieve type */
    type_bits = oid.hi & H5_DAOS_TYPE_MASK;
    if(type_bits == H5_DAOS_TYPE_GRP)
        return(H5I_GROUP);
    else if(type_bits == H5_DAOS_TYPE_DSET)
        return(H5I_DATASET);
    else if(type_bits == H5_DAOS_TYPE_DTYPE)
        return(H5I_DATATYPE);
    else if(type_bits == H5_DAOS_TYPE_MAP)
        return(H5I_MAP);
    else
        return(H5I_BADID);
} /* end H5_daos_oid_to_type() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_mult128
 *
 * Purpose:     Multiply two 128 bit unsigned integers to yield a 128 bit
 *              unsigned integer
 *
 * Return:      void
 *
 *-------------------------------------------------------------------------
 */
static void
H5_daos_mult128(uint64_t x_lo, uint64_t x_hi, uint64_t y_lo, uint64_t y_hi,
    uint64_t *ans_lo, uint64_t *ans_hi)
{
    uint64_t xlyl;
    uint64_t xlyh;
    uint64_t xhyl;
    uint64_t xhyh;
    uint64_t temp;

    assert(ans_lo);
    assert(ans_hi);

    /*
     * First calculate x_lo * y_lo
     */
    /* Compute 64 bit results of multiplication of each combination of high and
     * low 32 bit sections of x_lo and y_lo */
    xlyl = (x_lo & 0xffffffff) * (y_lo & 0xffffffff);
    xlyh = (x_lo & 0xffffffff) * (y_lo >> 32);
    xhyl = (x_lo >> 32) * (y_lo & 0xffffffff);
    xhyh = (x_lo >> 32) * (y_lo >> 32);

    /* Calculate lower 32 bits of the answer */
    *ans_lo = xlyl & 0xffffffff;

    /* Calculate second 32 bits of the answer. Use temp to keep a 64 bit result
     * of the calculation for these 32 bits, to keep track of overflow past
     * these 32 bits. */
    temp = (xlyl >> 32) + (xlyh & 0xffffffff) + (xhyl & 0xffffffff);
    *ans_lo += temp << 32;

    /* Calculate third 32 bits of the answer, including overflowed result from
     * the previous operation */
    temp >>= 32;
    temp += (xlyh >> 32) + (xhyl >> 32) + (xhyh & 0xffffffff);
    *ans_hi = temp & 0xffffffff;

    /* Calculate highest 32 bits of the answer. No need to keep track of
     * overflow because it has overflowed past the end of the 128 bit answer */
    temp >>= 32;
    temp += (xhyh >> 32);
    *ans_hi += temp << 32;

    /*
     * Now add the results from multiplying x_lo * y_hi and x_hi * y_lo. No need
     * to consider overflow here, and no need to consider x_hi * y_hi because
     * those results would overflow past the end of the 128 bit answer.
     */
    *ans_hi += (x_lo * y_hi) + (x_hi * y_lo);

    return;
} /* end H5_daos_mult128() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_hash128
 *
 * Purpose:     Hashes the string name to a 128 bit buffer (hash).
 *              Implementation of the FNV hash algorithm.
 *
 * Return:      void
 *
 *-------------------------------------------------------------------------
 */
void
H5_daos_hash128(const char *name, void *hash)
{
    const uint8_t *name_p = (const uint8_t *)name;
    uint8_t *hash_p = (uint8_t *)hash;
    uint64_t name_lo;
    uint64_t name_hi;
    /* Initialize hash value in accordance with the FNV algorithm */
    uint64_t hash_lo = 0x62b821756295c58d;
    uint64_t hash_hi = 0x6c62272e07bb0142;
    /* Initialize FNV prime number in accordance with the FNV algorithm */
    const uint64_t fnv_prime_lo = 0x13b;
    const uint64_t fnv_prime_hi = 0x1000000;
    size_t name_len_rem;

    assert(name);
    assert(hash);

    name_len_rem = strlen(name);

    while(name_len_rem > 0) {
        /* "Decode" lower 64 bits of this 128 bit section of the name, so the
         * numberical value of the integer is the same on both little endian and
         * big endian systems */
        if(name_len_rem >= 8) {
            UINT64DECODE(name_p, name_lo)
            name_len_rem -= 8;
        } /* end if */
        else {
            name_lo = 0;
            UINT64DECODE_VAR(name_p, name_lo, name_len_rem)
            name_len_rem = 0;
        } /* end else */

        /* "Decode" second 64 bits */
        if(name_len_rem > 0) {
            if(name_len_rem >= 8) {
                UINT64DECODE(name_p, name_hi)
                name_len_rem -= 8;
            } /* end if */
            else {
                name_hi = 0;
                UINT64DECODE_VAR(name_p, name_hi, name_len_rem)
                name_len_rem = 0;
            } /* end else */
        } /* end if */
        else
            name_hi = 0;

        /* FNV algorithm - XOR hash with name then multiply by fnv_prime */
        hash_lo ^= name_lo;
        hash_hi ^= name_hi;
        H5_daos_mult128(hash_lo, hash_hi, fnv_prime_lo, fnv_prime_hi, &hash_lo, &hash_hi);
    } /* end while */

    /* "Encode" hash integers to char buffer, so the buffer is the same on both
     * little endian and big endian systems */
    UINT64ENCODE(hash_p, hash_lo)
    UINT64ENCODE(hash_p, hash_hi)

    return;
} /* end H5_daos_hash128() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_tx_comp_cb
 *
 * Purpose:     Callback for daos_tx_commit()/abort() which closes the
 *              transaction.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2019
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_tx_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_req_t *req;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (req = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for transaction commit/abort task");

    /* Handle errors in commit/abort task.  Only record error in
     * udata->req_status if it does not already contain an error (it could
     * contain an error if another task this task is not dependent on also
     * failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = task->dt_result;
        req->failed_task = "transaction commit/abort";
    } /* end if */

    /* Close transaction */
    if(0 != (ret = daos_tx_close(req->th, NULL /*event*/)))
        D_GOTO_ERROR(H5E_IO, H5E_CLOSEERROR, ret, "can't close transaction: %s", H5_daos_err_to_string(ret));
    req->th_open = FALSE;

done:
    /* Complete finalize task in engine */
    tse_task_complete(req->finalize_task, ret_value);
    req->finalize_task = NULL;

    /* Make notify callback */
    if(req->notify_cb) {
        H5ES_status_t req_status;

        /* Determine request status */
        if(ret_value >= 0 && (req->status == -H5_DAOS_INCOMPLETE
                || req->status == -H5_DAOS_SHORT_CIRCUIT))
            req_status = H5ES_STATUS_SUCCEED;
        else if(req->status == -H5_DAOS_CANCELED)
            req_status = H5ES_STATUS_CANCELED;
        else
            req_status = H5ES_STATUS_FAIL;

        /* Make callback */
        if(req->notify_cb(req->notify_ctx, req_status) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CANTOPERATE, -H5_DAOS_CALLBACK_ERROR, "notify callback returned failure");
    } /* end if */

    /* Mark request as completed */
    if(ret_value >= 0 && (req->status == -H5_DAOS_INCOMPLETE
            || req->status == -H5_DAOS_SHORT_CIRCUIT))
        req->status = 0;

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = ret_value;
        req->failed_task = "transaction commit/abort completion callback";
    } /* end if */

    /* Release our reference to req */
    if(H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    D_FUNC_LEAVE;
} /* end H5_daos_tx_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_h5op_finalize
 *
 * Purpose:     Task function which is called when an HDF5 operation is
 *              complete.  Commits the transaction if one was opened for
 *              the operation, then releases its reference to req.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2019
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_h5op_finalize(tse_task_t *task)
{
    H5_daos_req_t *req;
    hbool_t close_tx = FALSE;
    int ret;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (req = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for operation finalize task");
    assert(task == req->finalize_task);

    /* Check for error */
    if(req->status < -H5_DAOS_SHORT_CIRCUIT) {
        /* Print error message */
        D_DONE_ERROR(H5E_IO, H5E_CANTINIT, req->status, "operation failed in task \"%s\": %s", req->failed_task, H5_daos_err_to_string(req->status));

        /* Abort transaction if opened */
        if(req->th_open) {
            tse_task_t *abort_task;
            daos_tx_abort_t *abort_args;

            /* Create task */
            if(0 != (ret = daos_task_create(DAOS_OPC_TX_ABORT, &req->file->sched, 0, NULL, &abort_task))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, ret, "can't create task to abort transaction: %s", H5_daos_err_to_string(ret));
            } /* end if */

            /* Set arguments */
            if(NULL == (abort_args = daos_task_get_args(abort_task))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for transaction abort task");
            } /* end if */
            abort_args->th = req->th;

            /* Register callback to close transaction */
            if(0 != (ret = tse_task_register_comp_cb(abort_task, H5_daos_tx_comp_cb, NULL, 0))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                tse_task_complete(abort_task, ret_value);
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, ret, "can't register callback to close transaction: %s", H5_daos_err_to_string(ret));
            } /* end if */

            /* Set private data for abort */
            (void)tse_task_set_priv(abort_task, req);

            /* Schedule abort task */
            if(0 != (ret = tse_task_schedule(abort_task, false))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                tse_task_complete(abort_task, ret_value);
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, ret, "can't schedule task to abort transaction: %s", H5_daos_err_to_string(ret));
            } /* end if */
            req->rc++;
        } /* end if */
    } /* end if */
    else {
        /* Commit transaction if opened */
        if(req->th_open) {
            tse_task_t *commit_task;
            daos_tx_commit_t *commit_args;

            /* Create task */
            if(0 != (ret = daos_task_create(DAOS_OPC_TX_COMMIT, &req->file->sched, 0, NULL, &commit_task))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, ret, "can't create task to commit transaction: %s", H5_daos_err_to_string(ret));
            } /* end if */

            /* Set arguments */
            if(NULL == (commit_args = daos_task_get_args(commit_task))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for transaction commit task");
            } /* end if */
            commit_args->th = req->th;

            /* Register callback to close transaction */
            if(0 != (ret = tse_task_register_comp_cb(commit_task, H5_daos_tx_comp_cb, NULL, 0))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                tse_task_complete(commit_task, ret_value);
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, ret, "can't register callback to close transaction: %s", H5_daos_err_to_string(ret));
            } /* end if */

            /* Set private data for commit */
            (void)tse_task_set_priv(commit_task, req);

            /* Schedule commit task */
            if(0 != (ret = tse_task_schedule(commit_task, false))) {
                close_tx = TRUE;
                req->th_open = FALSE;
                tse_task_complete(commit_task, ret_value);
                D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, ret, "can't schedule task to commit transaction: %s", H5_daos_err_to_string(ret));
            } /* end if */
            req->rc++;
        } /* end if */
    } /* end else */

done:
    if(req) {
        /* Check if we failed to start tx commit/abour task */
        if(close_tx) {
            /* Close transaction */
            if(0 != (ret = daos_tx_close(req->th, NULL /*event*/)))
                D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, ret, "can't close transaction: %s", H5_daos_err_to_string(ret));
            req->th_open = FALSE;
        } /* end if */

        /* Check if we're done */
        if(!req->th_open) {
            /* Make notify callback */
            if(req->notify_cb)
                if(req->notify_cb(req->notify_ctx, ret_value >= 0 && (req->status == -H5_DAOS_INCOMPLETE
                        || req->status == -H5_DAOS_SHORT_CIRCUIT) ? H5ES_STATUS_SUCCEED
                        : req->status == -H5_DAOS_CANCELED ? H5ES_STATUS_CANCELED : H5ES_STATUS_FAIL) < 0)
                    D_DONE_ERROR(H5E_VOL, H5E_CANTOPERATE, -H5_DAOS_CALLBACK_ERROR, "notify callback returned failure");

            /* Mark request as completed if there were no errors */
            if(ret_value >= 0 && (req->status == -H5_DAOS_INCOMPLETE
                    || req->status == -H5_DAOS_SHORT_CIRCUIT))
                req->status = 0;

            /* Complete task in engine */
            tse_task_complete(req->finalize_task, ret_value);
            req->finalize_task = NULL;
        } /* end if */
    } /* end if */
    else
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);

    /* Report failures in this routine */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = ret_value;
        req->failed_task = "h5 op finalize";
    } /* end if */

    /* Release our reference to req */
    if(H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    D_FUNC_LEAVE;
} /* end H5_daos_h5op_finalize() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_generic_prep_cb
 *
 * Purpose:     Prepare callback for generic DAOS operations.  Currently
 *              only checks for errors from previous tasks.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              February, 2020
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_generic_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_generic_cb_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for generic task");

    assert(udata->req);
    assert(udata->req->file);

    /* Handle errors */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

done:
    D_FUNC_LEAVE;
} /* end H5_daos_generic_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_generic_comp_cb
 *
 * Purpose:     Complete callback for generic DAOS operations.  Currently
 *              checks for a failed task then frees private data.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              February, 2020
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_generic_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_generic_cb_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for generic task");

    assert(udata->req);
    assert(udata->req->file);

    /* Handle errors in task.  Only record error in udata->req_status if it does
     * not already contain an error (it could contain an error if another task
     * this task is not dependent on also failed). */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = task->dt_result;
        udata->req->failed_task = udata->task_name;
    } /* end if */

done:
    if(udata) {
        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Free private data */
        DV_free(udata);
    }
    else
        assert(ret_value == -H5_DAOS_DAOS_GET_ERROR);

    D_FUNC_LEAVE;
} /* end H5_daos_generic_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_md_rw_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous daos_obj_update or
 *              daos_obj_fetch for metadata I/O.  Currently checks for
 *              errors from previous tasks then sets arguments for the
 *              DAOS operation.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2019
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_md_rw_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_md_rw_cb_ud_t *udata;
    daos_obj_rw_t *update_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for metadata I/O task");

    assert(udata->obj);
    assert(udata->req);
    assert(udata->obj->item.file);
    assert(!udata->obj->item.file->closed);

    /* Handle errors */
    if(udata->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Set update task arguments */
    if(NULL == (update_args = daos_task_get_args(task))) {
        tse_task_complete(task, -H5_DAOS_DAOS_GET_ERROR);
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for metadata I/O task");
    } /* end if */
    update_args->oh = udata->obj->obj_oh;
    update_args->th = udata->req->th;
    update_args->flags = 0;
    update_args->dkey = &udata->dkey;
    update_args->nr = udata->nr;
    update_args->iods = udata->iod;
    update_args->sgls = udata->sgl;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_md_rw_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_md_update_comp_cb
 *
 * Purpose:     Complete callback for asynchronous daos_obj_update for
 *              metadata writes.  Currently checks for a failed task then
 *              frees private data.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2019
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_md_update_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_md_rw_cb_ud_t *udata;
    unsigned i;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for metadata I/O task");
    assert(!udata->req->file->closed);

    /* Handle errors in update task.  Only record error in udata->req_status if
     * it does not already contain an error (it could contain an error if
     * another task this task is not dependent on also failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = task->dt_result;
        udata->req->failed_task = udata->task_name;
    } /* end if */

    /* Close object */
    if(H5_daos_object_close(udata->obj, H5I_INVALID_HID, NULL) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");

    /* Handle errors in this function */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = ret_value;
        udata->req->failed_task = udata->task_name;
    } /* end if */

    /* Release our reference to req */
    if(H5_daos_req_free_int(udata->req) < 0)
        D_DONE_ERROR(H5E_IO, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

    /* Free private data */
    if(udata->free_dkey)
        DV_free(udata->dkey.iov_buf);
    if(udata->free_akeys)
        for(i = 0; i < udata->nr; i++)
            DV_free(udata->iod[i].iod_name.iov_buf);
    for(i = 0; i < udata->nr; i++)
        if(udata->free_sg_iov[i])
            DV_free(udata->sg_iov[i].iov_buf);
    DV_free(udata);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_md_update_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_mpi_ibcast_task
 *
 * Purpose:     Wraps a call to MPI_Ibcast in a DAOS/TSE task.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2020
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_mpi_ibcast_task(tse_task_t *task)
{
    H5_daos_mpi_ibcast_ud_t *udata;
    int ret_value = 0;

    assert(!H5_daos_mpi_task_g);

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for MPI broadcast task");

    assert(udata->req);
    assert(udata->req->file);
    assert(!udata->req->file->closed);
    assert(udata->buffer);

    /* Make call to MPI_Ibcast */
    if(MPI_SUCCESS != MPI_Ibcast(udata->buffer, udata->count, MPI_BYTE, 0, udata->req->file->comm, &H5_daos_mpi_req_g))
        D_GOTO_ERROR(H5E_VOL, H5E_MPI, -H5_DAOS_MPI_ERROR, "MPI_Ibcast failed");

    /* Register this task as the current in-flight MPI task */
    H5_daos_mpi_task_g = task;

    /* This task will be completed by the progress function once that function
     * detects that the MPI request is finished */

done:
    D_FUNC_LEAVE;
} /* end H5_daos_mpi_ibcast_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_metatask_autocomp_other
 *
 * Purpose:     Body function for a metatask that needs to complete
 *              itself and another task.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              March, 2020
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_metatask_autocomp_other(tse_task_t *task)
{
    tse_task_t *other_task = NULL;
    int ret_value = 0;

    /* Get other task */
    if(NULL == (other_task = (tse_task_t *)tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for autocomplete other metatask");

    /* Complete other task */
    tse_task_complete(other_task, ret_value);

done:
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_metatask_autocomp_other() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_metatask_autocomplete
 *
 * Purpose:     Body function for a metatask that needs to complete
 *              itself.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              March, 2020
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_metatask_autocomplete(tse_task_t *task)
{
    tse_task_complete(task, 0);

    return 0;
} /* end H5_daos_metatask_autocomplete() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_list_key_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous daos key list
 *              operations.  Currently checks for errors from previous
 *              tasks then sets arguments for the DAOS operation.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2019
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_list_key_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_iter_ud_t *udata;
    daos_obj_list_t *list_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for key list task");

    assert(udata->target_obj);
    assert(udata->iter_data->req);
    assert(udata->iter_data->req->file);
    assert(!udata->iter_data->req->file->closed);

    /* Handle errors */
    if(udata->iter_data->req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->iter_data->req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Set oh argument */
    if(NULL == (list_args = daos_task_get_args(task))) {
        tse_task_complete(task, -H5_DAOS_DAOS_GET_ERROR);
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for key list task");
    } /* end if */
    list_args->oh = udata->target_obj->obj_oh;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_list_key_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_list_key_finish
 *
 * Purpose:     Frees key list udata and, if this is the base level of
 *              iteration, iter data.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              January, 2020
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_list_key_finish(tse_task_t *task)
{
    H5_daos_iter_ud_t *udata;
    H5_daos_req_t *req = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for iteration task");

    assert(task == udata->iter_metatask);

    /* Assign req convenience pointer.  We do this so we can still handle errors
     * after freeing.  This should be safe since we don't decrease the ref count
     * on req until we're done with it. */
    req = udata->iter_data->req;

    assert(req);
    assert(req->file);

    /* Finalize iter_data if this is the base of iteration */
    if(udata->base_iter) {
        /* Iteration is complete, we are no longer short-circuiting (if this
         * iteration caused the short circuit) */
        if(udata->iter_data->short_circuit_init) {
            if(udata->iter_data->req->status == -H5_DAOS_SHORT_CIRCUIT)
                udata->iter_data->req->status = -H5_DAOS_INCOMPLETE;
            udata->iter_data->short_circuit_init = FALSE;
        } /* end if */

        /* Decrement reference count on root obj id */
        if(H5Idec_ref(udata->iter_data->iter_root_obj) < 0)
            D_DONE_ERROR(H5E_LINK, H5E_CANTDEC, -H5_DAOS_H5_CLOSE_ERROR, "can't decrement reference count on iteration base object");
        udata->iter_data->iter_root_obj = H5I_INVALID_HID;

        /* Set *op_ret_p if present */
        if(udata->iter_data->op_ret_p)
            *udata->iter_data->op_ret_p = udata->iter_data->op_ret;

        /* Free hash table */
        if(udata->iter_data->iter_type == H5_DAOS_ITER_TYPE_LINK) {
            udata->iter_data->u.link_iter_data.recursive_link_path = DV_free(udata->iter_data->u.link_iter_data.recursive_link_path);

            if(udata->iter_data->u.link_iter_data.visited_link_table) {
                dv_hash_table_free(udata->iter_data->u.link_iter_data.visited_link_table);
                udata->iter_data->u.link_iter_data.visited_link_table = NULL;
            } /* end if */
        } /* end if */

        /* Free iter data */
        udata->iter_data = DV_free(udata->iter_data);
    } /* end if */
    else
        assert(udata->iter_data->is_recursive);
    
    /* Close target_obj */
    if(H5_daos_object_close(udata->target_obj, H5I_INVALID_HID, NULL) < 0)
        D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");

    /* Free buffer */
    if(udata->sg_iov.iov_buf)
        DV_free(udata->sg_iov.iov_buf);

    /* Free kds buffer if one was allocated */
    if(udata->kds_dyn)
        DV_free(udata->kds_dyn);

    /* Free udata */
    udata = DV_free(udata);

    /* Handle errors */
    /* Do not place any code that can issue errors after this block, except for
     * H5_daos_req_free_int, which updates req->status if it sees an error */
    if(ret_value < -H5_DAOS_SHORT_CIRCUIT && req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        req->status = ret_value;
        req->failed_task = "key list finish";
    } /* end if */

    /* Release req */
    if(H5_daos_req_free_int(req) < 0)
        D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

done:
    /* Mark task as complete */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_list_key_finish() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_list_key_start
 *
 * Purpose:     Begins listing keys (akeys or dkeys depending on opc)
 *              asynchronously, calling comp_cb when finished.  iter_udata
 *              must already be exist and be filled in with valid info.
 *              Can be used to continue iteration if the first call did
 *              not return all the keys.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_list_key_start(H5_daos_iter_ud_t *iter_udata, daos_opc_t opc,
    tse_task_cb_t comp_cb, tse_task_t **first_task, tse_task_t **dep_task)
{
    daos_obj_list_t *list_args;
    tse_task_t *list_task = NULL;
    int ret;
    int ret_value = 0;

    assert(iter_udata);
    assert(iter_udata->iter_metatask);
    assert(first_task);
    assert(dep_task);

    /* Create task for key list */
    if(0 != (ret = daos_task_create(opc, &iter_udata->target_obj->item.file->sched, *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &list_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create task to list keys: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for key list */
    if(0 != (ret = tse_task_register_cbs(list_task, H5_daos_list_key_prep_cb, NULL, 0, comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't register callbacks for task to list keys: %s", H5_daos_err_to_string(ret));

    /* Set private data for key list */
    (void)tse_task_set_priv(list_task, iter_udata);

    /* Get arguments for list operation */
    if(NULL == (list_args = daos_task_get_args(list_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for key list task");

    /* Set arguments */
    list_args->th = iter_udata->iter_data->req->th;
    iter_udata->nr = (uint32_t)iter_udata->kds_len;
    list_args->nr = &iter_udata->nr;
    list_args->kds = iter_udata->kds;
    list_args->sgl = &iter_udata->sgl;
    if(opc == DAOS_OPC_OBJ_LIST_DKEY)
        list_args->dkey_anchor = &iter_udata->anchor;
    else {
        assert(opc == DAOS_OPC_OBJ_LIST_AKEY);
        list_args->dkey = &iter_udata->dkey;
        list_args->akey_anchor = &iter_udata->anchor;
    } /* end if */

    /* Schedule list task (or save it to be scheduled later) and give it a
     * reference to req and target_obj */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(list_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't schedule task to list keys: %s", H5_daos_err_to_string(ret));
    }
    else
        *first_task = list_task;
    *dep_task = iter_udata->iter_metatask;
    iter_udata = NULL;

done:
    /* Cleanup */
    if(iter_udata) {
        assert(ret_value < 0);
        assert(iter_udata->iter_metatask);
        assert(iter_udata->sg_iov.iov_buf);

        if(*dep_task && 0 != (ret = tse_task_register_deps(iter_udata->iter_metatask, 1, dep_task)))
            D_DONE_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create dependencies for iteration metatask: %s", H5_daos_err_to_string(ret));

        if(*first_task) {
            if(0 != (ret = tse_task_schedule(iter_udata->iter_metatask, false)))
                D_DONE_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't schedule iteration metatask: %s", H5_daos_err_to_string(ret));
        } /* end if */
        else
            *first_task = iter_udata->iter_metatask;
        *dep_task = iter_udata->iter_metatask;
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_list_key_start() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_list_key_init
 *
 * Purpose:     Begins listing keys (akeys or dkeys depending on opc)
 *              asynchronously, calling comp_cb when finished.  Creates a
 *              metatask in the udata struct's "iter_metatask" field but
 *              does not schedule it.  It is the responsibility of comp_cb
 *              to make sure iter_metatask is scheduled such that it
 *              executes when everything is complete a this level of
 *              iteration.
 *
 *              key_prefetch_size specifies the number of keys to fetch at
 *              a time while prefetching keys during the listing operation.
 *              key_buf_size_init specifies the initial size in bytes of
 *              the buffer allocated to hold these keys. This buffer will
 *              be re-allocated as necessary if it is too small to hold the
 *              keys, but this may incur additional I/O overhead.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_list_key_init(H5_daos_iter_data_t *iter_data, H5_daos_obj_t *target_obj,
    daos_key_t *dkey, daos_opc_t opc, tse_task_cb_t comp_cb, hbool_t base_iter,
    size_t key_prefetch_size, size_t key_buf_size_init, tse_task_t **first_task,
    tse_task_t **dep_task)
{
    H5_daos_iter_ud_t *iter_udata = NULL;
    char *tmp_alloc = NULL;
    int ret;
    int ret_value = 0;

    assert(iter_data);
    assert(target_obj);
    assert(comp_cb);
    assert(key_prefetch_size > 0);
    assert(key_buf_size_init > 0);
    assert(first_task);
    assert(dep_task);

    /* Allocate iter udata */
    if(NULL == (iter_udata = (H5_daos_iter_ud_t *)DV_calloc(sizeof(H5_daos_iter_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate iteration user data");

    /* Fill in user data fields */
    iter_udata->target_obj = target_obj;
    if(dkey)
        iter_udata->dkey = *dkey;
    else
        assert(opc == DAOS_OPC_OBJ_LIST_DKEY);
    iter_udata->base_iter = base_iter;
    memset(&iter_udata->anchor, 0, sizeof(iter_udata->anchor));

    /* Copy iter_data if this is the base of iteration, otherwise point to
     * existing iter_data */
    if(base_iter) {
        if(NULL == (iter_udata->iter_data = (H5_daos_iter_data_t *)DV_malloc(sizeof(H5_daos_iter_data_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate iteration data");
        memcpy(iter_udata->iter_data, iter_data, sizeof(*iter_data));
    } /* end if */
    else
        iter_udata->iter_data = iter_data;

    /* Allocate kds buffer if necessary */
    iter_udata->kds = iter_udata->kds_static;
    iter_udata->kds_len = key_prefetch_size;
    if(key_prefetch_size * sizeof(daos_key_desc_t) > sizeof(iter_udata->kds_static)) {
        if(NULL == (iter_udata->kds_dyn = (daos_key_desc_t *)DV_malloc(key_prefetch_size * sizeof(daos_key_desc_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate key descriptor buffer");
        iter_udata->kds = iter_udata->kds_dyn;
    } /* end if */

    /* Allocate key_buf */
    if(NULL == (tmp_alloc = (char *)DV_malloc(key_buf_size_init)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, -H5_DAOS_ALLOC_ERROR, "can't allocate buffer for keys");

    /* Set up sg_iov.  Report size as 1 less than buffer size so we always have
     * room for a null terminator. */
    daos_iov_set(&iter_udata->sg_iov, tmp_alloc, (daos_size_t)(key_buf_size_init - 1));

    /* Set up sgl */
    iter_udata->sgl.sg_nr = 1;
    iter_udata->sgl.sg_nr_out = 0;
    iter_udata->sgl.sg_iovs = &iter_udata->sg_iov;

    /* Create meta task for iteration.  This empty task will be completed when
     * the iteration is finished by comp_cb.  We can't use list_task since it
     * may not be completed by the first list.  Only free iter_data at the end
     * if this is the base of iteration. */
    if(0 != (ret = tse_task_create(H5_daos_list_key_finish,
            &target_obj->item.file->sched, iter_udata, &iter_udata->iter_metatask)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create meta task for iteration: %s", H5_daos_err_to_string(ret));

    /* Start list (create tasks) give it a reference to req and target obj, and
     * transfer ownership of iter_udata */
    if(0 != (ret = H5_daos_list_key_start(iter_udata, opc, comp_cb, first_task, dep_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't start iteration");
    iter_udata->iter_data->req->rc++;
    iter_udata->target_obj->item.rc++;
    iter_udata = NULL;

done:
    /* Cleanup */
    if(iter_udata) {
        assert(ret_value < 0);

        if(iter_udata->iter_metatask) {
            /* The metatask should clean everything up */
            if(iter_udata->iter_metatask != *dep_task) {
                /* Queue up the metatask */
                if(*dep_task && 0 != (ret = tse_task_register_deps(iter_udata->iter_metatask, 1, dep_task)))
                    D_DONE_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create dependencies for iteration metatask: %s", H5_daos_err_to_string(ret));

                if(*first_task) {
                    if(0 != (ret = tse_task_schedule(iter_udata->iter_metatask, false)))
                        D_DONE_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't schedule iteration metatask: %s", H5_daos_err_to_string(ret));
                } /* end if */
                else
                    *first_task = iter_udata->iter_metatask;
                *dep_task = iter_udata->iter_metatask;
            } /* end if */
        } /* end if */
        else {
            /* No metatask, clean up directly here */
            /* Free iter_data if this is the base of iteration */
            if(iter_data->is_recursive && iter_udata->base_iter) {
                /* Free hash table */
                if(iter_data->iter_type == H5_DAOS_ITER_TYPE_LINK) {
                    iter_data->u.link_iter_data.recursive_link_path = DV_free(iter_data->u.link_iter_data.recursive_link_path);

                    if(iter_data->u.link_iter_data.visited_link_table) {
                        dv_hash_table_free(iter_data->u.link_iter_data.visited_link_table);
                        iter_data->u.link_iter_data.visited_link_table = NULL;
                    } /* end if */
                } /* end if */

                /* Free iter data */
                iter_udata->iter_data = DV_free(iter_udata->iter_data);
            } /* end if */

            /* Decrement reference count on root obj id */
            if(iter_udata->base_iter)
                if(H5Idec_ref(iter_data->iter_root_obj) < 0)
                    D_DONE_ERROR(H5E_VOL, H5E_CANTDEC, -H5_DAOS_H5_CLOSE_ERROR, "can't decrement reference count on iteration base object");

            /* Free key buffer */
            if(iter_udata->sg_iov.iov_buf)
                DV_free(iter_udata->sg_iov.iov_buf);

            /* Free kds buffer if one was allocated */
            if(iter_udata->kds_dyn)
                DV_free(iter_udata->kds_dyn);

            /* Free udata */
            iter_udata = DV_free(iter_udata);
        } /* end else */
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_list_key_init() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_obj_open_prep_cb
 *
 * Purpose:     Prepare callback for daos_obj_open.  Currently only sets
 *              the coh and checks for errors from previous tasks.  This
 *              is only necessary for operations that might otherwise be
 *              run before file->coh is set up, since daos_obj_open is a
 *              non-blocking operation.  The other fields in the argument
 *              struct must have already been filled in.  Since this does
 *              not hold the object open it must only be used when there
 *              is a task that depends on it that does so.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 * Programmer:  Neil Fortner
 *              February, 2020
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_obj_open_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_obj_open_ud_t *udata;
    daos_obj_open_t *open_args;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for object open task");

    assert(udata->generic_ud.req);
    assert(udata->file);

    /* Handle errors */
    if(udata->generic_ud.req->status < -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_PRE_ERROR);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_PRE_ERROR);
    } /* end if */
    else if(udata->generic_ud.req->status == -H5_DAOS_SHORT_CIRCUIT) {
        tse_task_complete(task, -H5_DAOS_SHORT_CIRCUIT);
        udata = NULL;
        D_GOTO_DONE(-H5_DAOS_SHORT_CIRCUIT);
    } /* end if */

    /* Set container open handle and oid in args */
    if(NULL == (open_args = daos_task_get_args(task))) {
        tse_task_complete(task, -H5_DAOS_DAOS_GET_ERROR);
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get arguments for object open task");
    } /* end if */
    open_args->coh = udata->file->coh;
    open_args->oid = *udata->oid;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_obj_open_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_obj_open
 *
 * Purpose:     Open a DAOS object object asynchronously.  daos_obj_open
 *              is a non-blocking call but it might be necessary to insert
 *              it into the scheduler so it doesn't run until certain
 *              conditions are met (such as the file's container handle
 *              being open).  oid must not point to memory that might be
 *              freed or go out of scope before the open task executes.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_obj_open(H5_daos_file_t *file, H5_daos_req_t *req, daos_obj_id_t *oid,
    unsigned mode, daos_handle_t *oh, const char *task_name,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    tse_task_t *open_task;
    H5_daos_obj_open_ud_t *open_udata = NULL;
    daos_obj_open_t *open_args;
    int ret;
    herr_t ret_value = SUCCEED; /* Return value */

    assert(file);
    assert(req);
    assert(oid);
    assert(first_task);
    assert(dep_task);

    /* Create task for object open */
    if(0 != (ret = daos_task_create(DAOS_OPC_OBJ_OPEN, &file->sched, *dep_task ? 1 : 0, *dep_task ? dep_task : NULL, &open_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to open object: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for object open */
    if(0 != (ret = tse_task_register_cbs(open_task, H5_daos_obj_open_prep_cb, NULL, 0, H5_daos_generic_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register callbacks for task to open object: %s", H5_daos_err_to_string(ret));

    /* Set private data for object open */
    if(NULL == (open_udata = (H5_daos_obj_open_ud_t *)DV_malloc(sizeof(H5_daos_obj_open_ud_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate user data struct for object open task");
    open_udata->generic_ud.req = req;
    open_udata->generic_ud.task_name = task_name;
    open_udata->file = file;
    open_udata->oid = oid;
    (void)tse_task_set_priv(open_task, open_udata);

    /* Set arguments for object open (oid will be set later by the prep
     * callback) */
    if(NULL == (open_args = daos_task_get_args(open_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't get arguments for object open task");
    open_args->mode = mode;
    open_args->oh = oh;

    /* Schedule object open task (or save it to be scheduled later) and give it
     * a reference to req */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(open_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to open object: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = open_task;
    req->rc++;
    open_udata = NULL;
    *dep_task = open_task;

done:
    /* Cleanup */
    if(open_udata) {
        assert(ret_value < 0);
        open_udata = DV_free(open_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_obj_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_mpi_ibcast
 *
 * Purpose:     Creates an asynchronous task for broadcasting a buffer.
 *              `_bcast_udata` may be NULL, in which case this routine will
 *              allocate a broadcast udata struct and assume an empty
 *              buffer is to be sent to trigger a failure on other
 *              processes. If `empty` is TRUE, the buffer will be memset
 *              with 0.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_mpi_ibcast(H5_daos_mpi_ibcast_ud_t *_bcast_udata, tse_sched_t *sched, H5_daos_obj_t *obj,
    size_t buffer_size, hbool_t empty, tse_task_cb_t bcast_prep_cb, tse_task_cb_t bcast_comp_cb,
    H5_daos_req_t *req, tse_task_t **first_task, tse_task_t **dep_task)
{
    H5_daos_mpi_ibcast_ud_t *bcast_udata = _bcast_udata;
    H5_daos_item_t *item = (H5_daos_item_t *)obj;
    tse_task_t *bcast_task;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(sched);
    assert(req);
    assert(first_task);
    assert(dep_task);

    /* Allocate bcast_udata if necessary */
    if(!bcast_udata) {
        if(NULL == (bcast_udata = (H5_daos_mpi_ibcast_ud_t *)DV_calloc(sizeof(H5_daos_mpi_ibcast_ud_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate buffer for MPI broadcast user data");
        bcast_udata->req = req;
        bcast_udata->obj = obj;
        bcast_udata->sched = sched;
    } /* end if */
    assert(bcast_udata->sched);

    /* Allocate bcast_udata's buffer if necessary */
    if(!bcast_udata->buffer) {
        if(NULL == (bcast_udata->buffer = DV_calloc(buffer_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "failed to allocate MPI broadcast buffer");
        bcast_udata->buffer_len = (int)buffer_size;
        bcast_udata->count = (int)buffer_size;
    } /* end if */
    else {
        assert(bcast_udata->buffer_len == (int)buffer_size);
        assert(bcast_udata->count == (int)buffer_size);
        if(empty)
            memset(bcast_udata->buffer, 0, buffer_size);
    } /* end else */

    /* Create meta task for bcast.  This empty task will be completed when
     * the bcast is finished by the completion callback. We can't use
     * bcast_task since it may not be completed after the first bcast. */
    if(0 != (ret = tse_task_create(NULL, sched, NULL, &bcast_udata->bcast_metatask)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create meta task for empty buffer broadcast: %s", H5_daos_err_to_string(ret));

    /* Create task for bcast */
    if(0 != (ret = tse_task_create(H5_daos_mpi_ibcast_task, sched, bcast_udata, &bcast_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to broadcast empty buffer: %s", H5_daos_err_to_string(ret));

    /* Register task dependency if present */
    if(*dep_task && 0 != (ret = tse_task_register_deps(bcast_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create dependencies for empty buffer broadcast task: %s", H5_daos_err_to_string(ret));

    /* Set callback functions for bcast */
    if(0 != (ret = tse_task_register_cbs(bcast_task, bcast_prep_cb, NULL, 0, bcast_comp_cb, NULL, 0)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register callbacks for empty buffer broadcast: %s", H5_daos_err_to_string(ret));

    /* Schedule meta task */
    if(0 != (ret = tse_task_schedule(bcast_udata->bcast_metatask, false)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule meta task for empty buffer broadcast: %s", H5_daos_err_to_string(ret));

    /* Schedule bcast task and transfer ownership of bcast_udata */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(bcast_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task for empty buffer broadcast: %s", H5_daos_err_to_string(ret));
        else {
            req->rc++;
            if(item) item->rc++;
            *dep_task = bcast_udata->bcast_metatask;
            bcast_udata = NULL;
        } /* end else */
    } /* end if */
    else {
        *first_task = bcast_task;
        req->rc++;
        if(item) item->rc++;
        *dep_task = bcast_udata->bcast_metatask;
        bcast_udata = NULL;
    } /* end else */

done:
    /* Cleanup on failure */
    if(bcast_udata) {
        DV_free(bcast_udata->buffer);
        DV_free(bcast_udata);
    } /* end if */

    D_FUNC_LEAVE;
} /* end H5_daos_mpi_ibcast() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_collective_error_check
 *
 * Purpose:     Creates an asynchronous task for broadcasting the status of
 *              a collective asynchronous operation. `_bcast_udata` may be
 *              NULL, in which case this routine will allocate a broadcast
 *              udata struct and assume an empty buffer is to be sent to
 *              trigger a failure on other processes.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_collective_error_check(H5_daos_obj_t *obj, tse_sched_t *sched, H5_daos_req_t *req,
    tse_task_t **first_task, tse_task_t **dep_task)
{
    herr_t ret_value = SUCCEED;

    assert(sched);
    assert(req);
    assert(req->file->num_procs > 1);
    assert(first_task);
    assert(dep_task);

    /* Setup the request's bcast udata structure for broadcasting the operation status */
    req->collective.coll_status = 0;
    req->collective.err_check_ud.req = req;
    req->collective.err_check_ud.obj = obj;
    req->collective.err_check_ud.sched = sched;
    req->collective.err_check_ud.buffer = &req->collective.coll_status;
    req->collective.err_check_ud.buffer_len = sizeof(req->collective.coll_status);
    req->collective.err_check_ud.count = req->collective.err_check_ud.buffer_len;
    req->collective.err_check_ud.bcast_metatask = NULL;

    if(H5_daos_mpi_ibcast(&req->collective.err_check_ud, sched, obj, sizeof(req->collective.coll_status),
            FALSE, (req->file->my_rank == 0) ? H5_daos_collective_error_check_prep_cb : NULL,
            H5_daos_collective_error_check_comp_cb, req, first_task, dep_task) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't broadcast collective operation status");

done:
    D_FUNC_LEAVE;
} /* end H5_daos_collective_error_check() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_collective_error_check_prep_cb
 *
 * Purpose:     Prepare callback for asynchronous MPI_Ibcast to broadcast
 *              the result status of a collective operation. Currently just
 *              sets the value for the status buffer on rank 0. Only meant
 *              to be called by the rank that is the root of broadcasting
 *              (usually rank 0).
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_collective_error_check_prep_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_mpi_ibcast_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for MPI broadcast task");

    assert(udata->req);
    assert(!udata->req->file->closed);
    assert(udata->req->file->my_rank == 0);
    assert(udata->buffer);
    assert(udata->buffer_len == sizeof(udata->req->status));

    *((int *)udata->buffer) = udata->req->status;

done:
    D_FUNC_LEAVE;
} /* end H5_daos_collective_error_check_prep_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_collective_error_check_comp_cb
 *
 * Purpose:     Complete callback for asynchronous MPI_Ibcast to broadcast
 *              the result status of a collective operation. Currently
 *              checks for a failed task, checks the status buffer to
 *              determine whether an error occurred on the broadcasting
 *              root rank, and then frees private data. Meant to be called
 *              by all ranks.
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_collective_error_check_comp_cb(tse_task_t *task, void H5VL_DAOS_UNUSED *args)
{
    H5_daos_mpi_ibcast_ud_t *udata;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (udata = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for MPI broadcast task");

    assert(udata->req);
    assert(udata->buffer);
    assert(udata->buffer_len == sizeof(udata->req->status));

    /* Handle errors in broadcast task.  Only record error in
     * udata->req_status if it does not already contain an error (it could
     * contain an error if another task this task is not dependent on also
     * failed). */
    if(task->dt_result < -H5_DAOS_PRE_ERROR
            && udata->req->status >= -H5_DAOS_SHORT_CIRCUIT) {
        udata->req->status = task->dt_result;
        udata->req->failed_task = "MPI_Ibcast of collective operation status";
    } /* end if */
    else if((task->dt_result == 0) &&
            (udata->req->file->my_rank != 0)) {
        int *status_buf = (int *)udata->buffer;

        assert(*status_buf != -H5_DAOS_PRE_ERROR);
        if((*status_buf) <= -H5_DAOS_H5_OPEN_ERROR) {
            udata->req->status = -H5_DAOS_REMOTE_ERROR;
            udata->req->failed_task = "remote task";
        } /* end if */
    } /* end else */

done:
    if(udata) {
        /* Close object */
        if(udata->obj && H5_daos_object_close(udata->obj, H5I_INVALID_HID, NULL) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_H5_CLOSE_ERROR, "can't close object");

        /* Release our reference to req */
        if(H5_daos_req_free_int(udata->req) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, -H5_DAOS_FREE_ERROR, "can't free request");

        /* Complete bcast metatask */
        tse_task_complete(udata->bcast_metatask, ret_value);
    } /* end if */
    else
        assert(ret_value >= 0 || ret_value == -H5_DAOS_DAOS_GET_ERROR);

    D_FUNC_LEAVE;
} /* end H5_daos_collective_error_check_comp_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_free_async_task
 *
 * Purpose:     Frees a buffer (the private data).
 *
 * Return:      Success:        0
 *              Failure:        Error code
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_free_async_task(tse_task_t *task)
{
    void *buf = NULL;
    int ret_value = 0;

    assert(!H5_daos_mpi_task_g);

    /* Get private data */
    if(NULL == (buf = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_IO, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for free task");

    /* Free buffer */
    DV_free(buf);

done:
    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_free_async_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_free_async
 *
 * Purpose:     Schedules a task to free a buffer.  Executes even if a
 *              previous task failed, does not issue new failures.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_free_async(H5_daos_file_t *file, void *buf, tse_task_t **first_task,
    tse_task_t **dep_task)
{
    tse_task_t *free_task;
    int ret;
    herr_t ret_value = SUCCEED; /* Return value */

    assert(file);
    assert(buf);
    assert(first_task);
    assert(dep_task);

    /* Create task for free */
    if(0 != (ret = tse_task_create(H5_daos_free_async_task, &file->sched, buf, &free_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create task to free buffer: %s", H5_daos_err_to_string(ret));

    /* Register dependency for task */
    if(*dep_task && 0 != (ret = tse_task_register_deps(free_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create dependencies for free: %s", H5_daos_err_to_string(ret));

    /* Schedule free task (or save it to be scheduled later) */
    if(*first_task) {
        if(0 != (ret = tse_task_schedule(free_task, false)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't schedule task to free buffer: %s", H5_daos_err_to_string(ret));
    } /* end if */
    else
        *first_task = free_task;

    /* Do not update *dep_task since nothing depends on this buffer being freed
     */

done:
    D_FUNC_LEAVE;
} /* end H5_daos_free_async() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_sched_link_old_task
 *
 * Purpose:     Asynchronous task for H5_daos_sched_link().  Exists in
 *              old_sched, completes the new task in new_sched.
 *
 * Return:      0 on success/Negative error code on failure
 *
 *-------------------------------------------------------------------------
 */
static int
H5_daos_sched_link_old_task(tse_task_t *task)
{
    tse_task_t *new_task = NULL;
    int ret_value = 0;

    /* Get private data */
    if(NULL == (new_task = tse_task_get_priv(task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, -H5_DAOS_DAOS_GET_ERROR, "can't get private data for sched link task");

    /* Complete new task */
    tse_task_complete(new_task, 0);

done:
    /* Complete this task */
    tse_task_complete(task, ret_value);

    D_FUNC_LEAVE;
} /* end H5_daos_sched_link_old_task() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_sched_link
 *
 * Purpose:     Switches a task dependency chain from old_sched to
 *              new_sched.  *dep_task must be in old_sched on entry, and
 *              on exit dep_task will be a task in new_sched that will
 *              complete as soon as the original *dep_task completes.
 *
 * Return:      0 on success/Negative error code on failure
 *
 *-------------------------------------------------------------------------
 */
int
H5_daos_sched_link(tse_sched_t *old_sched, tse_sched_t *new_sched,
    tse_task_t **dep_task)
{
    tse_task_t *old_task = NULL;
    tse_task_t *new_task = NULL;
    int ret;
    int ret_value = 0;

    assert(dep_task);
    assert(*dep_task);

    /* If the schedulers are the same no need to do anything */
    if(old_sched == new_sched)
        D_GOTO_DONE(0);

    /* Create empty task in new scheduler - this will be returned in *dep_task,
     * and will be completed by old task when it runs */
    if(0 != (ret = tse_task_create(NULL, new_sched, NULL, &new_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create new task to link schedulers: %s", H5_daos_err_to_string(ret));;

    /* Schedule new task */
    if(0 != (ret = tse_task_schedule(new_task, false)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't schedule new task to link schedulers: %s", H5_daos_err_to_string(ret));

    /* Create task in old scheduler */
    if(0 != (ret = tse_task_create(H5_daos_sched_link_old_task, old_sched, new_task, &old_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create old task to link schedulers: %s", H5_daos_err_to_string(ret));

    /* Register dependency for old task */
    if(0 != (ret = tse_task_register_deps(old_task, 1, dep_task)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't create dependencies for old task to link schedulers: %s", H5_daos_err_to_string(ret));

    /* Schedule old task */
    if(0 != (ret = tse_task_schedule(old_task, false)))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, ret, "can't schedule old task to link schedulers: %s", H5_daos_err_to_string(ret));

    /* Update *dep_task to be the new task */
    *dep_task = new_task;
done:
    D_FUNC_LEAVE;
} /* end H5_daos_sched_link() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_progress
 *
 * Purpose:     Make progress on asynchronous tasks.  Can be run with a
 *              request (in which case it waits until the the request
 *              finishes) or without one (in which case it waits until all
 *              tasks in the file are complete.  Can be run with timeout
 *              set to H5_DAOS_PROGRESS_KICK in which case it makes
 *              non-blocking progress then exits immediately, with timout
 *              set to H5_DAOS_PROGRESS_WAIT in which case it waits as
 *              long as it takes, or with timeout set to a value in
 *              microseconds in which case it wait up to that amount of
 *              time then exits as soon as the exit condition or the
 *              timeout is met.
 *
 * Return:      Success:    Non-negative.
 *
 *              Failure:    Negative.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_progress(tse_sched_t *sched, H5_daos_req_t *req, uint64_t timeout)
{
    int64_t  timeout_rem;
    int      completed;
    bool     is_empty = FALSE;
    tse_task_t *tmp_task;
    int      ret;
    herr_t   ret_value = SUCCEED;

    assert(sched);

    /* Set timeout_rem, being careful to avoid overflow */
    timeout_rem = timeout > INT64_MAX ? INT64_MAX : (int64_t)timeout;

    /* Loop until the scheduler is empty, the timeout is met, or  */
    do {
        /* Progress MPI if there is a task in flight */
        if(H5_daos_mpi_task_g) {
            /* Check if task is complete */
            if(MPI_SUCCESS != (ret = MPI_Test(&H5_daos_mpi_req_g, &completed, MPI_STATUS_IGNORE)))
                D_DONE_ERROR(H5E_VOL, H5E_MPI, FAIL, "MPI_Test failed: %d", ret);

            /* Complete matching DAOS task if so */
            if(ret_value < 0) {
                tmp_task = H5_daos_mpi_task_g;
                H5_daos_mpi_task_g = NULL;
                tse_task_complete(tmp_task, -H5_DAOS_MPI_ERROR);
            } /* end if */
            else if(completed) {
                tmp_task = H5_daos_mpi_task_g;
                H5_daos_mpi_task_g = NULL;
                tse_task_complete(tmp_task, 0);
            } /* end if */
        } /* end if */

        /* Progress DAOS */
        if((0 != (ret = daos_progress(sched,
                timeout_rem > H5_DAOS_ASYNC_POLL_INTERVAL ? H5_DAOS_ASYNC_POLL_INTERVAL : timeout_rem,
                &is_empty))) && (ret != -DER_TIMEDOUT))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't progress scheduler: %s", H5_daos_err_to_string(ret));

        /* Advance time */
        /* Actually check clock here? */
        timeout_rem -= H5_DAOS_ASYNC_POLL_INTERVAL;
    } while((req ? (req->status == -H5_DAOS_INCOMPLETE || req->status == -H5_DAOS_SHORT_CIRCUIT)
            : !is_empty) && timeout_rem > 0);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_progress() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_progress_2
 *
 * Purpose:     Like H5_daos_progress except operates on two schedulers at
 *              once (for cross-file operations).
 *
 * Return:      Success:    Non-negative.
 *
 *              Failure:    Negative.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_progress_2(tse_sched_t *sched1, tse_sched_t *sched2, H5_daos_req_t *req,
    uint64_t timeout)
{
    int64_t  timeout_rem;
    int      completed;
    bool     is_empty1 = FALSE;
    bool     is_empty2 = FALSE;
    tse_task_t *tmp_task;
    int      ret;
    herr_t   ret_value = SUCCEED;

    assert(sched1);
    assert(sched2);

    /* Set timeout_rem, being careful to avoid overflow */
    timeout_rem = timeout > INT64_MAX ? INT64_MAX : (int64_t)timeout;

    /* Loop until the scheduler is empty, the timeout is met, or  */
    do {
        /* Progress MPI if there is a task in flight */
        if(H5_daos_mpi_task_g) {
            /* Check if task is complete */
            if(MPI_SUCCESS != (ret = MPI_Test(&H5_daos_mpi_req_g, &completed, MPI_STATUS_IGNORE)))
                D_DONE_ERROR(H5E_VOL, H5E_MPI, FAIL, "MPI_Test failed: %d", ret);

            /* Complete matching DAOS task if so */
            if(ret_value < 0) {
                tmp_task = H5_daos_mpi_task_g;
                H5_daos_mpi_task_g = NULL;
                tse_task_complete(tmp_task, -H5_DAOS_MPI_ERROR);
            } /* end if */
            else if(completed) {
                tmp_task = H5_daos_mpi_task_g;
                H5_daos_mpi_task_g = NULL;
                tse_task_complete(tmp_task, 0);
            } /* end if */
        } /* end if */

        /* Progress DAOS */
        if((0 != (ret = daos_progress(sched1,
                timeout_rem > H5_DAOS_ASYNC_POLL_INTERVAL ? H5_DAOS_ASYNC_POLL_INTERVAL : timeout_rem,
                &is_empty1))) && (ret != -DER_TIMEDOUT))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't progress scheduler 1: %s", H5_daos_err_to_string(ret));

        /* Advance time */
        /* Actually check clock here? */
        timeout_rem -= H5_DAOS_ASYNC_POLL_INTERVAL;

        /* Progress DAOS */
        if((0 != (ret = daos_progress(sched2,
                timeout_rem > H5_DAOS_ASYNC_POLL_INTERVAL ? H5_DAOS_ASYNC_POLL_INTERVAL : timeout_rem,
                &is_empty1))) && (ret != -DER_TIMEDOUT))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't progress scheduler 2: %s", H5_daos_err_to_string(ret));

        /* Advance time */
        /* Actually check clock here? */
        timeout_rem -= H5_DAOS_ASYNC_POLL_INTERVAL;
    } while((req ? (req->status == -H5_DAOS_INCOMPLETE || req->status == -H5_DAOS_SHORT_CIRCUIT)
            : !(is_empty1 && is_empty2)) && timeout_rem > 0);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_progress_2() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_comm_info_dup
 *
 * Purpose:     Make duplicates of MPI communicator and info objects.
 *              If the info object is in fact MPI_INFO_NULL, no duplicate
 *              is made but the same value is assigned to the 'info_new'
 *              object handle.
 *
 * Return:      Success:    Non-negative.  The new communicator and info
 *                          object handles are returned via the comm_new
 *                          and info_new pointers.
 *
 *              Failure:    Negative.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_comm_info_dup(MPI_Comm comm, MPI_Info info,
    MPI_Comm *comm_new, MPI_Info *info_new)
{
    MPI_Comm comm_dup = MPI_COMM_NULL;
    MPI_Info info_dup = MPI_INFO_NULL;
    int      mpi_code;
    herr_t   ret_value = SUCCEED;

    /* Check arguments */
    if(MPI_COMM_NULL == comm)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "invalid MPI communicator -- MPI_COMM_NULL");
    if(!comm_new)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "comm_new pointer is NULL");
    if(!info_new)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "info_new pointer is NULL");

    /* Duplicate the MPI objects. Temporary variables are used for error recovery cleanup. */
    if(MPI_SUCCESS != (mpi_code = MPI_Comm_dup(comm, &comm_dup)))
        D_GOTO_ERROR(H5E_INTERNAL, H5E_MPI, FAIL, "MPI_Comm_dup failed: %d", mpi_code);
    if(MPI_INFO_NULL != info) {
        if(MPI_SUCCESS != (mpi_code = MPI_Info_dup(info, &info_dup)))
            D_GOTO_ERROR(H5E_INTERNAL, H5E_MPI, FAIL, "MPI_Info_dup failed: %d", mpi_code);
    }
    else {
        info_dup = info;
    }

    /* Set MPI_ERRORS_RETURN on comm_dup so that MPI failures are not fatal,
       and return codes can be checked and handled. May 23, 2017 FTW */
    if(MPI_SUCCESS != (mpi_code = MPI_Comm_set_errhandler(comm_dup, MPI_ERRORS_RETURN)))
        D_GOTO_ERROR(H5E_INTERNAL, H5E_MPI, FAIL, "MPI_Comm_set_errhandler failed: %d", mpi_code);

    /* Copy the duplicated MPI objects to the return arguments. */
    *comm_new = comm_dup;
    *info_new = info_dup;

done:
    if(FAIL == ret_value) {
        /* Need to free anything created */
        if(MPI_COMM_NULL != comm_dup)
            MPI_Comm_free(&comm_dup);
        if(MPI_INFO_NULL != info_dup)
            MPI_Info_free(&info_dup);
    }

    D_FUNC_LEAVE;
} /* end H5_daos_comm_info_dup() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_comm_info_free
 *
 * Purpose:     Free the MPI communicator and info objects.
 *              If comm or info is in fact MPI_COMM_NULL or MPI_INFO_NULL,
 *              respectively, no action occurs to it.
 *
 * Return:      Success:    Non-negative.  The values the pointers refer
 *                          to will be set to the corresponding NULL
 *                          handles.
 *
 *              Failure:    Negative.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_comm_info_free(MPI_Comm *comm, MPI_Info *info)
{
    herr_t ret_value = SUCCEED;

    /* Check arguments. */
    if(!comm)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "comm pointer is NULL");
    if(!info)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "info pointer is NULL");

    if(MPI_COMM_NULL != *comm)
        MPI_Comm_free(comm);
    if(MPI_INFO_NULL != *info)
        MPI_Info_free(info);

done:
    D_FUNC_LEAVE;
} /* end H5_daos_comm_info_free() */


H5PL_type_t
H5PLget_plugin_type(void) {
    return H5PL_TYPE_VOL;
}


const void*
H5PLget_plugin_info(void) {
    return &H5_daos_g;
}
