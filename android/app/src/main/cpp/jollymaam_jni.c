/* libjollymaam for the Android app: its hooks over the Kotlin NativeUsb the
 * app opened (one USB device: kind 0 our tool device, 1 the 8BitDo boot
 * loader, 2 a Jieli ROM loader).  The pad is followed across its restarts
 * (into our updater, into its vendor loader) by NativeUsb.follow: each look
 * at USB asks which device and kind it is now, our firmware only with the
 * UID picked. */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jollymaam.h"
#include "package.h"

struct bridge {
  JNIEnv *env;
  jobject transport;
  jmethodID scsi, write, read, set, wait, follow, generation, size, progress, say, block;
  int kind, has_uid;
  uint8_t uid[16];
};

static int prepare(struct bridge *b, JNIEnv *env, jobject transport, int kind) {
  memset(b, 0, sizeof *b);
  b->env = env, b->transport = transport, b->kind = kind;
  if (!transport) return 0;
  jclass cls = (*env)->GetObjectClass(env, transport);
  if (!cls) return 0;
  b->scsi = (*env)->GetMethodID(env, cls, "scsi", "([BI[BI)I");
  b->write = (*env)->GetMethodID(env, cls, "hidWrite", "([B)I");
  b->read = (*env)->GetMethodID(env, cls, "hidRead", "([BI)I");
  b->set = (*env)->GetMethodID(env, cls, "featureSet", "([B)I");
  b->wait = (*env)->GetMethodID(env, cls, "waitMillis", "(I)V");
  b->follow = (*env)->GetMethodID(env, cls, "follow", "([BI)I");
  b->generation = (*env)->GetMethodID(env, cls, "generation", "()I");
  b->block = (*env)->GetMethodID(env, cls, "backupBlock", "(J[B)Z");
  b->size = (*env)->GetMethodID(env, cls, "featureSize", "()I");
  b->progress = (*env)->GetMethodID(env, cls, "onProgress", "(JJ)V");
  b->say = (*env)->GetMethodID(env, cls, "onSay", "(Ljava/lang/String;)V");
  (*env)->DeleteLocalRef(env, cls);
  return !(*env)->ExceptionCheck(env);
}

/* A Kotlin call that threw: cleared and a failure for the library.  (A
 * pending exception aborts the app at the next JNI call: a USB error during
 * a restart is expected, the library retries or fails the operation.) */
static int threw(JNIEnv *e) {
  if (!(*e)->ExceptionCheck(e)) return 0;
  (*e)->ExceptionClear(e);
  return 1;
}

static jbyteArray bytes(JNIEnv *env, const uint8_t *data, size_t size) {
  jbyteArray r = (*env)->NewByteArray(env, (jsize)size);
  if (r && size && data) (*env)->SetByteArrayRegion(env, r, 0, (jsize)size, (const jbyte *)data);
  return r;
}

/* A byte-array call: its count, with read's bytes copied back. */
static int call(struct bridge *b, jmethodID method, uint8_t *data, size_t size, int timeout) {
  JNIEnv *e = b->env;
  if ((*e)->ExceptionCheck(e)) return -1;
  jbyteArray a = bytes(e, data, size);
  if (!a) return -1;
  jint n = timeout >= 0 ? (*e)->CallIntMethod(e, b->transport, method, a, (jint)timeout)
                        : (*e)->CallIntMethod(e, b->transport, method, a);
  if (threw(e) || n < 0 || (size_t)n > size) n = -1;
  else if (method == b->read) (*e)->GetByteArrayRegion(e, a, 0, n, (jbyte *)data);
  (*e)->DeleteLocalRef(e, a);
  return n;
}

/* ---- the hooks -------------------------------------------------------------------------------------- */

static int list(void *ctx, int (*visit)(void *, const struct jm_device_info *), void *user) {
  struct bridge *b = ctx;
  JNIEnv *e = b->env;
  struct jm_device_info d;
  /* The device now: the one opened while it stays, else what took its place
   * (a new generation, so a restart asked for is seen as done). */
  jbyteArray uid = b->has_uid ? bytes(e, b->uid, 16) : NULL;
  if (b->has_uid && !uid) return threw(e), 0;
  jint kind = (*e)->CallIntMethod(e, b->transport, b->follow, uid, (jint)b->kind);
  if (threw(e)) kind = -1;
  if (uid) (*e)->DeleteLocalRef(e, uid);
  if (kind < 0 || kind > 2) return 0;
  jint generation = (*e)->CallIntMethod(e, b->transport, b->generation);
  if (threw(e)) return 0;
  b->kind = kind;
  memset(&d, 0, sizeof d);
  snprintf(d.path, sizeof d.path, "android");
  snprintf(d.id, sizeof d.id, "android#%d", (int)generation);
  if (b->kind == 2) d.kind = JM_DEVICE_DISK;
  else if (b->kind == 1) d.vid = 0x2dc8, d.pid = 0x3208, d.output_size = 64;
  else {
    d.vid = 0x16c0, d.pid = 0x05df, d.usage_page = 0xffc2;
    d.feature_size = (uint16_t)(*e)->CallIntMethod(e, b->transport, b->size);
  }
  visit(user, &d);
  return 1;
}
static void *open_dev(void *ctx, const struct jm_device_info *d) { (void)d; return ctx; }
static void close_dev(void *ctx, void *dev) { (void)ctx, (void)dev; }
/* Kotlin's hidWrite takes the report without its ID byte (0). */
static int write_report(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)dev;
  if (!n || r[0]) return -1;
  int k = call(ctx, ((struct bridge *)ctx)->write, (uint8_t *)r + 1, n - 1, -1);
  return k < 0 ? -1 : k + 1;
}
static int read_report(void *ctx, void *dev, uint8_t *out, size_t n, unsigned timeout) {
  (void)dev;
  memset(out, 0, n);
  return call(ctx, ((struct bridge *)ctx)->read, out, n, (int)timeout);
}
static int set_feature(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)dev;
  return call(ctx, ((struct bridge *)ctx)->set, (uint8_t *)r, n, -1);
}
static int scsi(void *ctx, void *dev, const uint8_t *cdb, size_t cdb_size, enum jm_scsi_direction dir,
                uint8_t *data, size_t size, unsigned timeout, size_t *done) {
  struct bridge *b = ctx;
  JNIEnv *e = b->env;
  (void)dev;
  if ((*e)->ExceptionCheck(e)) return -1;
  jbyteArray c = bytes(e, cdb, cdb_size), a = bytes(e, dir == JM_SCSI_OUT ? data : NULL, size);
  jint n = c && a ? (*e)->CallIntMethod(e, b->transport, b->scsi, c, (jint)dir, a, (jint)timeout) : -1;
  int ok = !threw(e) && n >= 0 && (size_t)n <= size;
  if (ok && dir == JM_SCSI_IN && n) (*e)->GetByteArrayRegion(e, a, 0, n, (jbyte *)data);
  if (c) (*e)->DeleteLocalRef(e, c);
  if (a) (*e)->DeleteLocalRef(e, a);
  if (ok) *done = (size_t)n;
  return ok ? 0 : -1;
}
static void nap(void *ctx, unsigned ms) {
  struct bridge *b = ctx;
  if (!(*b->env)->ExceptionCheck(b->env)) (*b->env)->CallVoidMethod(b->env, b->transport, b->wait, (jint)ms);
  threw(b->env);
}
/* The library's lines to Kotlin's onSay (logcat, the Flash panel), as
 * ASCII: NewStringUTF takes modified UTF-8 only. */
static void say(void *ctx, const char *line) {
  struct bridge *b = ctx;
  JNIEnv *e = b->env;
  char ascii[400];
  size_t n = 0;
  for (; line[n] && n < sizeof ascii - 1; n++) ascii[n] = (unsigned char)line[n] < 0x80 ? line[n] : '?';
  ascii[n] = 0;
  if ((*e)->ExceptionCheck(e)) return;
  jstring s = (*e)->NewStringUTF(e, ascii);
  if (s) (*e)->CallVoidMethod(e, b->transport, b->say, s), threw(e), (*e)->DeleteLocalRef(e, s);
  else threw(e);
}
static void progress(void *ctx, const char *what, size_t done, size_t total) {
  struct bridge *b = ctx;
  (void)what;
  if (!(*b->env)->ExceptionCheck(b->env))
    (*b->env)->CallVoidMethod(b->env, b->transport, b->progress, (jlong)done, (jlong)total);
  threw(b->env);
}

static void start(struct jm *jm, struct jm_hooks *h, struct bridge *b) {
  memset(h, 0, sizeof *h);
  h->ctx = b, h->list = list, h->open = open_dev, h->close = close_dev;
  h->write = write_report, h->read = read_report, h->set_feature = set_feature, h->scsi = scsi;
  h->sleep = nap, h->say = say, h->progress = progress;
  memset(jm, 0, sizeof *jm);
  jm->h = h;
}

/* ---- the firmware package ------------------------------------------------------------------------- */

struct package { jbyte *data; jbyteArray array; struct jm_package zip; struct jm_firmware fw; };
static int open_package(JNIEnv *e, jbyteArray input, struct package *p) {
  char error[128];
  memset(p, 0, sizeof *p);
  if (!input || (*e)->GetArrayLength(e, input) > 64 * 1024 * 1024) return 0;
  p->array = input;
  if (!(p->data = (*e)->GetByteArrayElements(e, input, NULL))) return 0;
  return jm_package_open(&p->zip, (uint8_t *)p->data, (size_t)(*e)->GetArrayLength(e, input)) == JM_PACKAGE_OK &&
         jm_firmware_open(&p->fw, jm_zip_read, &p->zip, error);
}
static void close_package(JNIEnv *e, struct package *p) {
  if (p->data) (*e)->ReleaseByteArrayElements(e, p->array, p->data, JNI_ABORT);
}

/* ---- NativeCore ------------------------------------------------------------------------------------------ */

/* The package's products: product, board, chip, three strings each. */
JNIEXPORT jobjectArray JNICALL Java_dev_jollymaam_app_NativeCore_products(JNIEnv *e, jobject self, jbyteArray input) {
  struct package p;
  const struct jm_product *found[JM_FIRMWARE_FILES];
  unsigned n = 0;
  jobjectArray result = NULL;
  (void)self;
  if (open_package(e, input, &p))
    for (unsigned i = 0; i < p.fw.count; i++) {
      const struct jm_product *pr = jm_product(p.fw.files[i].name);
      int dup = 0;
      for (unsigned k = 0; k < n; k++) dup |= found[k] == pr;
      if (pr && pr->board && !dup) found[n++] = pr;
    }
  jclass string = n ? (*e)->FindClass(e, "java/lang/String") : NULL;
  if (string) result = (*e)->NewObjectArray(e, (jsize)n * 3, string, NULL);
  for (unsigned i = 0; result && i < n && !(*e)->ExceptionCheck(e); i++) {
    const char *values[] = {found[i]->pid, found[i]->board, jm_chip_name(found[i]->chip)};
    for (int k = 0; k < 3; k++) {
      jstring v = (*e)->NewStringUTF(e, values[k]);
      if (!v) break;
      (*e)->SetObjectArrayElement(e, result, (jsize)(i * 3 + k), v);
      (*e)->DeleteLocalRef(e, v);
    }
  }
  if (string) (*e)->DeleteLocalRef(e, string);
  close_package(e, &p);
  return result;
}

static int probed(void *user, const struct jm_found *f) {
  *(struct jm_found *)user = *f;
  return 1;
}

/* The device is ours with this UID (the one the user picked). */
static int same_uid(struct jm *jm, struct bridge *b) {
  struct jm_found f;
  memset(&f, 0, sizeof f);
  int ok = jm_scan(jm, probed, &f) == 1 && (f.kind == JM_FOUND_OURS || f.kind == JM_FOUND_UPDATER) &&
           !memcmp(f.id.uid, b->uid, 16);
  return ok;
}

/* What the device is: kind 0 its IDENTIFY (88 bytes), 1 the boot loader's
 * model (2 bytes), 2 the Jieli chip's name. */
JNIEXPORT jbyteArray JNICALL Java_dev_jollymaam_app_NativeCore_probe(JNIEnv *e, jobject self, jobject transport, jint kind) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct jm_found f;
  uint8_t data[88] = {0};
  size_t size = 0;
  (void)self;
  if (!prepare(&b, e, transport, kind)) return NULL;
  start(&jm, &h, &b);
  memset(&f, 0, sizeof f);
  if (jm_scan(&jm, probed, &f) != 1) return NULL;
  if (kind == 0 && (f.kind == JM_FOUND_OURS || f.kind == JM_FOUND_UPDATER)) {
    memcpy(data, "C2PD", 4), data[4] = (uint8_t)f.id.protocol, data[5] = (uint8_t)(f.id.protocol >> 8);
    data[6] = f.id.board, data[7] = f.id.updater;
    memcpy(data + 8, f.id.build, 16), memcpy(data + 24, f.id.uid, 16), memcpy(data + 40, f.id.product, 16);
    memcpy(data + 56, f.id.board_name, 32), size = 88;
  } else if (kind == 1 && f.kind == JM_FOUND_8BITDO_LOADER) {
    data[0] = (uint8_t)f.product->model, data[1] = (uint8_t)(f.product->model >> 8), size = 2;
  } else if (kind == 2 && f.kind == JM_FOUND_JIELI_LOADER) {
    memcpy(data, jm_chip_name(f.chip), 4), size = 4;
  }
  return size && !(*e)->ExceptionCheck(e) ? bytes(e, data, size) : NULL;
}

/* The package's firmware for product onto the device: kind 0 through our
 * updater (found again by uid after its restart), or with install its whole
 * install image (updater and firmware) through the vendor loader it is
 * restarted into; a loader (kind 1, 2) through itself. */
JNIEXPORT jint JNICALL Java_dev_jollymaam_app_NativeCore_run(JNIEnv *e, jobject self, jbyteArray input, jstring product,
                                                              jint kind, jbyteArray uid, jboolean install,
                                                              jobject transport) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct package p;
  int result = -100;
  (void)self;
  memset(&p, 0, sizeof p);
  if (!product || kind < 0 || kind > 2 || (kind == 0 && (!uid || (*e)->GetArrayLength(e, uid) != 16)) ||
      !prepare(&b, e, transport, kind))
    return -100;
  if (uid) (*e)->GetByteArrayRegion(e, uid, 0, 16, (jbyte *)b.uid), b.has_uid = 1;
  const char *name = (*e)->GetStringUTFChars(e, product, NULL);
  if (name && open_package(e, input, &p)) {
    struct jm_select sel = {NULL, name[0] ? name : NULL, 0, 0};   /* "": a Jieli loader's flash names it */
    start(&jm, &h, &b);
    if (kind == 0 && !same_uid(&jm, &b)) result = -2;
    else result = kind == 0 && !install ? jm_update(&jm, &p.fw, &sel) : jm_oemflash(&jm, &p.fw, &sel);
  }
  if (name) (*e)->ReleaseStringUTFChars(e, product, name);
  close_package(e, &p);
  return result;
}

/* Our firmware with this UID into its vendor loader (the app rescans for it). */
JNIEXPORT jint JNICALL Java_dev_jollymaam_app_NativeCore_stockLoader(JNIEnv *e, jobject self, jbyteArray uid, jobject transport) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct jm_select sel = {NULL, NULL, 0, 1};
  (void)self;
  if (!uid || (*e)->GetArrayLength(e, uid) != 16 || !prepare(&b, e, transport, 0)) return -1;
  (*e)->GetByteArrayRegion(e, uid, 0, 16, (jbyte *)b.uid);
  start(&jm, &h, &b);
  if (!same_uid(&jm, &b)) return -2;
  return jm_loader(&jm, &sel);
}

/* The library's backup blocks to Kotlin's backupBlock (a file it keeps). */
static int to_kotlin(void *user, uint32_t address, const uint8_t *data, size_t size) {
  struct bridge *b = user;
  JNIEnv *e = b->env;
  if ((*e)->ExceptionCheck(e)) return 0;
  jbyteArray a = bytes(e, data, size);
  jboolean ok = a && (*e)->CallBooleanMethod(e, b->transport, b->block, (jlong)address, a);
  if (threw(e)) ok = 0;
  if (a) (*e)->DeleteLocalRef(e, a);
  return ok;
}

/* The whole flash, read through the vendor loader (our firmware, uid given,
 * is restarted into it first, and stays there); the package gives a Jieli
 * loader its RAM loader. */
JNIEXPORT jint JNICALL Java_dev_jollymaam_app_NativeCore_backup(JNIEnv *e, jobject self, jbyteArray input, jint kind,
                                                                 jbyteArray uid, jobject transport) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct package p;
  struct jm_select sel = {NULL, NULL, 0, 0};
  int result = -100;
  (void)self;
  if (kind < 0 || kind > 2 || (kind == 0 && (!uid || (*e)->GetArrayLength(e, uid) != 16)) ||
      !prepare(&b, e, transport, kind))
    return -100;
  if (uid) (*e)->GetByteArrayRegion(e, uid, 0, 16, (jbyte *)b.uid), b.has_uid = 1;
  if (open_package(e, input, &p)) {
    start(&jm, &h, &b);
    result = kind == 0 && !same_uid(&jm, &b) ? -2 : jm_backup(&jm, &p.fw, &sel, to_kotlin, &b);
  }
  close_package(e, &p);
  return result;
}

/* A vendor loader out of itself: the firmware it holds starts. */
JNIEXPORT jint JNICALL Java_dev_jollymaam_app_NativeCore_start(JNIEnv *e, jobject self, jbyteArray input, jint kind,
                                                                jobject transport) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct package p;
  struct jm_select sel = {NULL, NULL, 0, 0};
  int result = -100;
  (void)self;
  if (kind < 1 || kind > 2 || !prepare(&b, e, transport, kind)) return -100;
  if (open_package(e, input, &p)) {
    start(&jm, &h, &b);
    result = jm_start(&jm, &p.fw, &sel);
  }
  close_package(e, &p);
  return result;
}
