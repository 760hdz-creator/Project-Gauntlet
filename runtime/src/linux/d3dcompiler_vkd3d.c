/* Linux: D3DCompile on vkd3d-shader (HLSL -> DXBC for DXVK), with a minimal ID3DBlob in C. */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <vkd3d_shader.h>

typedef struct { ID3D10Blob iface; LONG refs; SIZE_T size; char data[]; } Blob;

static HRESULT STDMETHODCALLTYPE blob_qi(ID3D10Blob *b, REFIID iid, void **out) { (void)iid; *out = b; ID3D10Blob_AddRef(b); return S_OK; }
static ULONG STDMETHODCALLTYPE blob_addref(ID3D10Blob *b) { return (ULONG)__atomic_add_fetch(&((Blob *)b)->refs, 1, __ATOMIC_SEQ_CST); }
static ULONG STDMETHODCALLTYPE blob_release(ID3D10Blob *b) {
    LONG r = __atomic_sub_fetch(&((Blob *)b)->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) free(b);
    return (ULONG)r;
}
static void *STDMETHODCALLTYPE blob_ptr(ID3D10Blob *b) { return ((Blob *)b)->data; }
static SIZE_T STDMETHODCALLTYPE blob_size(ID3D10Blob *b) { return ((Blob *)b)->size; }
static const ID3D10BlobVtbl blob_vtbl = {blob_qi, blob_addref, blob_release, blob_ptr, blob_size};

static ID3D10Blob *blob_new(const void *p, size_t n) {
    Blob *b = malloc(sizeof *b + n + 1);
    b->iface.lpVtbl = (ID3D10BlobVtbl *)&blob_vtbl;
    b->refs = 1;
    b->size = n;
    memcpy(b->data, p, n);
    b->data[n] = 0;
    return &b->iface;
}

HRESULT WINAPI D3DCompile(const void *data, SIZE_T data_size, const char *filename, const D3D_SHADER_MACRO *defines,
                          ID3DInclude *include, const char *entrypoint, const char *target, UINT flags1, UINT flags2,
                          ID3DBlob **shader, ID3DBlob **errors) {
    (void)flags1; (void)flags2;
    if (shader) *shader = NULL;
    if (errors) *errors = NULL;
    if (!data || !shader || !target || defines || include) return E_INVALIDARG;
    struct vkd3d_shader_hlsl_source_info hlsl = {VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO, NULL, entrypoint, {0}, target};
    struct vkd3d_shader_compile_info info = {0};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = data;
    info.source.size = data_size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = filename;
    struct vkd3d_shader_code out = {0};
    char *msg = NULL;
    int r = vkd3d_shader_compile(&info, &out, &msg);
    if (errors && msg && *msg) *errors = blob_new(msg, strlen(msg));
    if (msg) vkd3d_shader_free_messages(msg);
    if (!r) *shader = blob_new(out.code, out.size);
    if (out.code) vkd3d_shader_free_shader_code(&out);
    return r ? E_FAIL : S_OK;
}
