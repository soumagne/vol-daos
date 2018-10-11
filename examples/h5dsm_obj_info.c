#include "h5dsm_example.h"

int main(int argc, char *argv[]) {
    uuid_t pool_uuid;
    char *pool_grp = NULL;
    hid_t file = -1, obj = -1, fapl = -1;
    H5O_info_t oinfo;
    char *obj_str = NULL;
    H5VL_daosm_snap_id_t snap_id;

    (void)MPI_Init(&argc, &argv);

    if(argc < 4 || argc > 5)
        PRINTF_ERROR("argc must be 4 or 5\n");

    /* Parse UUID */
    if(0 != uuid_parse(argv[1], pool_uuid))
        ERROR;

    /* Initialize VOL */
    if(H5VLdaosm_init(MPI_COMM_WORLD, pool_uuid, pool_grp) < 0)
        ERROR;

    /* Set up FAPL */
    if((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        ERROR;
    if(H5Pset_fapl_daosm(fapl, MPI_COMM_WORLD, MPI_INFO_NULL) < 0)
        ERROR;
    if(H5Pset_all_coll_metadata_ops(fapl, true) < 0)
        ERROR;

    /* Open snapshot if specified */
    if(argc == 5) {
        snap_id = (H5VL_daosm_snap_id_t)atoi(argv[4]);
        printf("Opening snapshot %llu\n", (long long unsigned)snap_id);
        if(H5Pset_daosm_snap_open(fapl, snap_id) < 0)
            ERROR;
    } /* end if */

    /* Open file */
    if((file = H5Fopen(argv[2], H5F_ACC_RDONLY, fapl)) < 0)
        ERROR;

    printf("Opening object\n");

    /* Open object */
    if((obj = H5Oopen(file, argv[3], H5P_DEFAULT)) < 0)
        ERROR;

    /* Get object info */
    if(H5Oget_info(obj, &oinfo) < 0)
        ERROR;

    printf("fileno = %lu\n", oinfo.fileno);
    printf("addr = 0x%016llx\n", (unsigned long long)oinfo.addr);
    if(oinfo.type == H5O_TYPE_GROUP)
        obj_str = "group";
    else if(oinfo.type == H5O_TYPE_DATASET)
        obj_str = "dataset";
    else if(oinfo.type == H5O_TYPE_NAMED_DATATYPE)
        obj_str = "datatype";
    else if(oinfo.type == H5I_MAP)
        obj_str = "map";
    else
        obj_str = "unknown";
    printf("Object type is %s\n", obj_str);

    /* Close */
    if(H5Oclose(obj) < 0)
        ERROR;
    if(H5Fclose(file) < 0)
        ERROR;
    if(H5Pclose(fapl) < 0)
        ERROR;

    printf("Success\n");

    (void)MPI_Finalize();
    return 0;

error:
    H5E_BEGIN_TRY {
        H5Oclose(obj);
        H5Fclose(file);
        H5Pclose(fapl);
    } H5E_END_TRY;

    (void)MPI_Finalize();
    return 1;
}

