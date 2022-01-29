#include <string.h>
#include "erl_nif.h"
#include "concurrentqueue.h"
#include <vector>

struct NifTraits : public moodycamel::ConcurrentQueueDefaultTraits {
    static const size_t BLOCK_SIZE = 16;
    static const size_t EXPLICIT_BLOCK_EMPTY_COUNTER_THRESHOLD = 16;
    static const size_t EXPLICIT_INITIAL_INDEX_SIZE = 8;
    static const size_t IMPLICIT_INITIAL_INDEX_SIZE = 8;
    static const size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 16;
    static const std::uint32_t EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE = 256;

    static inline void *malloc(std::size_t size) { return enif_alloc(size); }

    static inline void free(void *ptr) { enif_free(ptr); }
};

using lfqIns = moodycamel::ConcurrentQueue<ErlNifBinary, NifTraits> *;

const size_t BulkDelCnt = 200;

ERL_NIF_TERM atomOk;
ERL_NIF_TERM atomError;
ERL_NIF_TERM atomNewErr;
ERL_NIF_TERM atomTrue;
ERL_NIF_TERM atomFalse;
ERL_NIF_TERM atomEmpty;

void eLfqFree(ErlNifEnv *, void *obj) {
    lfqIns *ObjIns = static_cast<lfqIns *>(obj);

    if (NULL != ObjIns && NULL != *ObjIns) {
        std::vector <ErlNifBinary> TermBinList(BulkDelCnt);
        size_t OutSize;
        do{
            OutSize = (*ObjIns)->try_dequeue_bulk(TermBinList.begin(), TermBinList.size());
            for (int i = OutSize - 1; i >= 0; i--) {
                enif_release_binary(&TermBinList[i]);
            }
        }while(OutSize >= BulkDelCnt);

        delete (*ObjIns);
        *ObjIns = NULL;
    }
}

int nifLoad(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM) {
    ErlNifResourceFlags flags = static_cast<ErlNifResourceFlags>(ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER);
    ErlNifResourceType *ResIns = NULL;
    ResIns = enif_open_resource_type(env, NULL, "eLfqRes", eLfqFree, flags, NULL);
    if (NULL == ResIns)
        return -1;

    *priv_data = ResIns;
    atomOk = enif_make_atom(env, "ok");
    atomTrue = enif_make_atom(env, "true");
    atomFalse = enif_make_atom(env, "false");
    atomError = enif_make_atom(env, "lfq_error");
    atomEmpty = enif_make_atom(env, "lfq_empty");
    atomNewErr = enif_make_atom(env, "error");
    return 0;
}

int nifUpgrade(ErlNifEnv *env, void **priv_data, void **, ERL_NIF_TERM) {
    ErlNifResourceFlags flags = static_cast<ErlNifResourceFlags>(ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER);
    ErlNifResourceType *ResIns = NULL;
    ResIns = enif_open_resource_type(env, NULL, "eLfqRes", eLfqFree, flags, NULL);
    if (NULL == ResIns)
        return -1;

    *priv_data = ResIns;
    return 0;
}

void nifUnload(ErlNifEnv *, void *priv_data) {
}

ERL_NIF_TERM nifNew(ErlNifEnv *env, int, const ERL_NIF_TERM *) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));

    lfqIns *ObjIns = static_cast<lfqIns *>(enif_alloc_resource(ResIns, sizeof(lfqIns)));
    *ObjIns = new moodycamel::ConcurrentQueue<ErlNifBinary, NifTraits>;

    if (ObjIns == NULL)
        return atomNewErr;

    if (*ObjIns == NULL)
        return atomNewErr;

    ERL_NIF_TERM RefTerm = enif_make_resource(env, ObjIns);
    enif_release_resource(ObjIns);
    return enif_make_tuple2(env, atomOk, RefTerm);
}

ERL_NIF_TERM nifDel1(ErlNifEnv *env, int, const ERL_NIF_TERM argv[]) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));

    lfqIns *ObjIns = NULL;

    if (!enif_get_resource(env, argv[0], ResIns, (void **) &ObjIns)) {
        return enif_make_badarg(env);
    }

    if (NULL != ObjIns && NULL != *ObjIns) {
        std::vector <ErlNifBinary> TermBinList(BulkDelCnt);
        size_t OutSize;
        do{
            OutSize = (*ObjIns)->try_dequeue_bulk(TermBinList.begin(), TermBinList.size());
            for (int i = OutSize - 1; i >= 0; i--) {
                enif_release_binary(&TermBinList[i]);
            }
        }while(OutSize >= BulkDelCnt);

        delete (*ObjIns);
        *ObjIns = NULL;
    }

    return atomOk;
}

ERL_NIF_TERM nifIn2(ErlNifEnv *env, int, const ERL_NIF_TERM argv[]) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));

    lfqIns *ObjIns = NULL;

    if (!enif_get_resource(env, argv[0], ResIns, (void **) &ObjIns) || NULL == *ObjIns) {
        return enif_make_badarg(env);
    }

    ErlNifBinary TermBin;
    if (!enif_term_to_binary(env, argv[1], &TermBin))
        return enif_make_badarg(env);

    if ((*ObjIns)->enqueue(TermBin)) {
        return atomTrue;
    } else {
        enif_release_binary(&TermBin);
        return atomFalse;
    }
}

ERL_NIF_TERM nifIns2(ErlNifEnv *env, int, const ERL_NIF_TERM argv[]) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));

    lfqIns *ObjIns = NULL;

    if (!enif_get_resource(env, argv[0], ResIns, (void **) &ObjIns) || NULL == *ObjIns) {
        return enif_make_badarg(env);
    }

    ERL_NIF_TERM List;
    ERL_NIF_TERM Head;

    List = argv[1];
    if (!enif_is_list(env, List)) {
        return enif_make_badarg(env);
    }

    unsigned ListLen;
    enif_get_list_length(env, List, &ListLen);

    std::vector <ErlNifBinary> TermBinList;

    ErlNifBinary TermBin;
    while (enif_get_list_cell(env, List, &Head, &List)) {
        if (!enif_term_to_binary(env, Head, &TermBin)) {
            for (auto it = TermBinList.begin(); it != TermBinList.end(); ++it) {
                enif_release_binary(&(*it));
            }
            return enif_make_badarg(env);
        }

        TermBinList.push_back(TermBin);
    }

    if ((*ObjIns)->enqueue_bulk(TermBinList.cbegin(), TermBinList.size())) {
        return atomTrue;
    } else {
        for (auto it = TermBinList.begin(); it != TermBinList.end(); ++it) {
            enif_release_binary(&(*it));
        }
        return atomFalse;
    }
}

ERL_NIF_TERM nifTryOut1(ErlNifEnv *env, int, const ERL_NIF_TERM argv[]) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));
    lfqIns *ObjIns = NULL;

    if (!enif_get_resource(env, argv[0], ResIns, (void **) &ObjIns) || NULL == *ObjIns) {
        return enif_make_badarg(env);
    }

    ErlNifBinary TermBin;

    if ((*ObjIns)->try_dequeue(TermBin)) {
        ERL_NIF_TERM OutTerm;
        if (enif_binary_to_term(env, TermBin.data, TermBin.size, &OutTerm, 0) == 0) {
            enif_release_binary(&TermBin);
            return atomError;
        } else {
            enif_release_binary(&TermBin);
            return OutTerm;
        }
    } else {
        return atomEmpty;
    }
}

ERL_NIF_TERM nifTryOuts2(ErlNifEnv *env, int, const ERL_NIF_TERM argv[]) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));
    lfqIns *ObjIns = NULL;

    if (!enif_get_resource(env, argv[0], ResIns, (void **) &ObjIns) || NULL == *ObjIns) {
        return enif_make_badarg(env);
    }

    unsigned int OutLen;
    if (!enif_get_uint(env, argv[1], &OutLen)) {
        return enif_make_badarg(env);
    }

    std::vector <ErlNifBinary> TermBinList(OutLen);
    size_t OutSize = (*ObjIns)->try_dequeue_bulk(TermBinList.begin(), TermBinList.size());

    ERL_NIF_TERM RetList = enif_make_list(env, 0);

    ERL_NIF_TERM OutTerm;
    for (int i = OutSize - 1; i >= 0; i--) {
        if (enif_binary_to_term(env, TermBinList[i].data, TermBinList[i].size, &OutTerm, 0) == 0) {
            enif_release_binary(&TermBinList[i]);
        } else {
            enif_release_binary(&TermBinList[i]);
            RetList = enif_make_list_cell(env, OutTerm, RetList);
        }
    }
    return RetList;
}

ERL_NIF_TERM nifSize1(ErlNifEnv *env, int, const ERL_NIF_TERM argv[]) {
    ErlNifResourceType *ResIns = static_cast<ErlNifResourceType *>(enif_priv_data(env));
    lfqIns *ObjIns = NULL;

    if (!enif_get_resource(env, argv[0], ResIns, (void **) &ObjIns) || NULL == *ObjIns) {
        return enif_make_badarg(env);
    }

    size_t LfqSize = (*ObjIns)->size_approx();
    return enif_make_long(env, (long int) LfqSize);
}

static ErlNifFunc nifFuncs[] =
        {
                {"new",     0, nifNew},
                {"del",     1, nifDel1},
                {"in",      2, nifIn2},
                {"ins",     2, nifIns2},
                {"tryOut",  1, nifTryOut1},
                {"tryOuts", 2, nifTryOuts2},
                {"size",    1, nifSize1}
        };

ERL_NIF_INIT(eLfq, nifFuncs, nifLoad, NULL, nifUpgrade, nifUnload)