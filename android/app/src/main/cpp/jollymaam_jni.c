/* libjollymaam for the Android app: its hooks over the Kotlin NativeUsb the
 * app opened (one USB device: kind 0 our tool device, 1 the 8BitDo boot
 * loader, 2 a Jieli ROM loader, 3 a pad on stock firmware, each of its HID
 * interfaces listed for the library to pick the one taking its command).  The pad is followed across its restarts
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
  jmethodID identity, interfaces, descriptor, select;
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
  b->identity = (*env)->GetMethodID(env, cls, "identity", "()[Ljava/lang/String;");
  b->interfaces = (*env)->GetMethodID(env, cls, "hidInterfaces", "()I");
  b->descriptor = (*env)->GetMethodID(env, cls, "hidDescriptor", "(I)[B");
  b->select = (*env)->GetMethodID(env, cls, "hidSelect", "(I)Z");
  b->progress = (*env)->GetMethodID(env, cls, "onProgress", "(Ljava/lang/String;JJ)V");
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

static void copy_string(JNIEnv *e, jobjectArray a, jsize i, char *out, size_t size) {
  jstring s = (*e)->GetObjectArrayElement(e, a, i);
  const char *c = s ? (*e)->GetStringUTFChars(e, s, NULL) : NULL;
  snprintf(out, size, "%s", c ? c : "");
  if (c) (*e)->ReleaseStringUTFChars(e, s, c);
  if (s) (*e)->DeleteLocalRef(e, s);
}

/* A stock pad: each HID interface, its report descriptor read (path
 * "android:N", N the interface for open to select); one port, so the
 * library keeps the interface that takes its command. */
static int stock_interfaces(struct bridge *b, struct jm_device_info *d,
                            int (*visit)(void *, const struct jm_device_info *), void *user) {
  JNIEnv *e = b->env;
  jobjectArray id = (*e)->CallObjectMethod(e, b->transport, b->identity);
  if (threw(e) || !id || (*e)->GetArrayLength(e, id) != 5) return 0;
  char number[16];
  copy_string(e, id, 0, number, sizeof number), d->vid = (uint16_t)strtoul(number, NULL, 16);
  copy_string(e, id, 1, number, sizeof number), d->pid = (uint16_t)strtoul(number, NULL, 16);
  copy_string(e, id, 2, number, sizeof number), d->bcd = (uint16_t)strtoul(number, NULL, 16);
  copy_string(e, id, 3, d->manufacturer, sizeof d->manufacturer);
  copy_string(e, id, 4, d->product, sizeof d->product);
  (*e)->DeleteLocalRef(e, id);
  d->kind = JM_DEVICE_HID;
  snprintf(d->port, sizeof d->port, "android");
  jint n = (*e)->CallIntMethod(e, b->transport, b->interfaces);
  if (threw(e)) return 0;
  int count = 0;
  for (jint i = 0; i < n; i++) {
    struct jm_device_info one = *d;
    jbyteArray r = (*e)->CallObjectMethod(e, b->transport, b->descriptor, i);
    if (threw(e)) return count;
    if (r) {
      uint8_t buf[1024];
      jsize k = (*e)->GetArrayLength(e, r);
      if (k > (jsize)sizeof buf) k = sizeof buf;
      (*e)->GetByteArrayRegion(e, r, 0, k, (jbyte *)buf);
      (*e)->DeleteLocalRef(e, r);
      jm_hid_descriptor(buf, (size_t)k, &one);
    }
    snprintf(one.path, sizeof one.path, "android:%d", (int)i);
    count++;
    if (visit(user, &one)) break;
  }
  return count;
}

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
  if (kind < 0 || kind > 3) return 0;
  jint generation = (*e)->CallIntMethod(e, b->transport, b->generation);
  if (threw(e)) return 0;
  b->kind = kind;
  memset(&d, 0, sizeof d);
  snprintf(d.path, sizeof d.path, "android");
  snprintf(d.id, sizeof d.id, "android#%d", (int)generation);
  if (b->kind == 3) return stock_interfaces(b, &d, visit, user);
  if (b->kind == 2) d.kind = JM_DEVICE_DISK;
  else if (b->kind == 1) d.vid = 0x2dc8, d.pid = 0x3208, d.output_size = 64;
  else {
    d.vid = 0x16c0, d.pid = 0x05df, d.usage_page = 0xffc2;
    d.feature_size = (uint16_t)(*e)->CallIntMethod(e, b->transport, b->size);
  }
  visit(user, &d);
  return 1;
}
static void *open_dev(void *ctx, const struct jm_device_info *d) {
  struct bridge *b = ctx;
  if (strncmp(d->path, "android:", 8)) return ctx;
  jboolean ok = (*b->env)->CallBooleanMethod(b->env, b->transport, b->select, (jint)atoi(d->path + 8));
  return threw(b->env) || !ok ? NULL : ctx;
}
static void close_dev(void *ctx, void *dev) { (void)ctx, (void)dev; }
/* As hidraw takes it: the report ID first (0: none, not sent), the stock
 * pads' commands on IDs of their own (0x81, 0x0f). */
static int write_report(void *ctx, void *dev, const uint8_t *r, size_t n) {
  (void)dev;
  if (!n) return -1;
  return call(ctx, ((struct bridge *)ctx)->write, (uint8_t *)r, n, -1);
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
  JNIEnv *e = b->env;
  if ((*e)->ExceptionCheck(e)) return;
  jstring s = (*e)->NewStringUTF(e, what ? what : "");
  if (s) (*e)->CallVoidMethod(e, b->transport, b->progress, s, (jlong)done, (jlong)total), (*e)->DeleteLocalRef(e, s);
  threw(e);
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

/* The package's products: product, board, chip, name, how to enter its
 * vendor loader by hand ("" none), five strings each. */
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
  if (string) result = (*e)->NewObjectArray(e, (jsize)n * 5, string, NULL);
  for (unsigned i = 0; result && i < n && !(*e)->ExceptionCheck(e); i++) {
    const char *values[] = {found[i]->pid, found[i]->board, jm_chip_name(found[i]->chip), found[i]->name,
                            found[i]->by_hand ? found[i]->by_hand : ""};
    for (int k = 0; k < 5; k++) {
      jstring v = (*e)->NewStringUTF(e, values[k]);
      if (!v) break;
      (*e)->SetObjectArrayElement(e, result, (jsize)(i * 5 + k), v);
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

static jobjectArray strings(JNIEnv *e, const char *const *values, int n) {
  jclass string = (*e)->FindClass(e, "java/lang/String");
  jobjectArray result = string ? (*e)->NewObjectArray(e, n, string, NULL) : NULL;
  for (int k = 0; result && k < n; k++) {
    jstring v = (*e)->NewStringUTF(e, values[k]);
    if (!v) return NULL;
    (*e)->SetObjectArrayElement(e, result, k, v);
    (*e)->DeleteLocalRef(e, v);
  }
  if (string) (*e)->DeleteLocalRef(e, string);
  return result;
}

static void hex(char *out, const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* What the device is (jm_scan), as strings: what (jm_found_what), product
 * ID, board, product name, chip, stock mode, build, flash ID (hex), "1" in
 * our updater, "1" when a command restarts it into its loader, what to do
 * by hand.  "" where not known; NULL: nothing the library knows. */
JNIEXPORT jobjectArray JNICALL Java_dev_jollymaam_app_NativeCore_probe(JNIEnv *e, jobject self, jobject transport, jint kind) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct jm_found f;
  char uid[33] = "", build[17] = "";
  (void)self;
  if (!prepare(&b, e, transport, kind)) return NULL;
  start(&jm, &h, &b);
  memset(&f, 0, sizeof f);
  if (jm_scan(&jm, probed, &f) < 1) return NULL;
  if (f.kind == JM_FOUND_OURS || f.kind == JM_FOUND_UPDATER) {
    hex(uid, f.id.uid, 16);
    memcpy(build, f.id.build, 16);
  }
  const struct jm_product *p = f.product;
  const char *values[] = {jm_found_what(&f), p ? p->pid : "", p && p->board ? p->board : "", p ? p->name : "",
                          f.kind == JM_FOUND_JIELI_LOADER || p ? jm_chip_name(f.chip) : "", f.mode ? f.mode : "",
                          build, uid, f.kind == JM_FOUND_UPDATER ? "1" : "", f.can_enter ? "1" : "",
                          p && p->by_hand ? p->by_hand : ""};
  return (*e)->ExceptionCheck(e) ? NULL : strings(e, values, 11);
}

/* The package's update.bin for product through our updater (our firmware
 * with uid restarted into it, found again by uid). */
JNIEXPORT jint JNICALL Java_dev_jollymaam_app_NativeCore_update(JNIEnv *e, jobject self, jbyteArray input, jstring product,
                                                                 jbyteArray uid, jobject transport) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct package p;
  int result = -100;
  (void)self;
  if (!product || !uid || (*e)->GetArrayLength(e, uid) != 16 || !prepare(&b, e, transport, 0)) return -100;
  (*e)->GetByteArrayRegion(e, uid, 0, 16, (jbyte *)b.uid), b.has_uid = 1;
  const char *name = (*e)->GetStringUTFChars(e, product, NULL);
  if (name && open_package(e, input, &p)) {
    struct jm_select sel = {NULL, name, 0, 0};
    start(&jm, &h, &b);
    result = same_uid(&jm, &b) ? jm_update(&jm, &p.fw, &sel) : -2;
  }
  if (name) (*e)->ReleaseStringUTFChars(e, product, name);
  close_package(e, &p);
  return result;
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

/* One pad session (jm_pad: the pad restarted into its vendor loader once,
 * a Jieli RAM loader uploaded once): with backup the whole flash to
 * backupBlock first, then the action: 0 none (it stays in its loader), 1
 * install the package's image, 2 restore image (raw: written whole), 3
 * reboot.  uid: our firmware, the one picked.  0 done; else the step that
 * failed: -1 the pad, -2 another pad, -3 the backup, -4 the action. */
JNIEXPORT jint JNICALL Java_dev_jollymaam_app_NativeCore_session(JNIEnv *e, jobject self, jbyteArray input, jint kind,
                                                                  jbyteArray uid, jboolean backup, jint action,
                                                                  jbyteArray image, jboolean raw, jobject transport) {
  struct bridge b;
  struct jm_hooks h;
  struct jm jm;
  struct package p;
  struct jm_found f;
  struct jm_pad pad;
  int result = -100;
  (void)self;
  if (kind < 0 || kind > 3 || action < 0 || action > 3 || (action == 2 && !image) ||
      (uid && (*e)->GetArrayLength(e, uid) != 16) || !prepare(&b, e, transport, kind))
    return -100;
  if (uid) (*e)->GetByteArrayRegion(e, uid, 0, 16, (jbyte *)b.uid), b.has_uid = 1;
  if (!open_package(e, input, &p)) {
    close_package(e, &p);
    return -100;
  }
  start(&jm, &h, &b);
  memset(&f, 0, sizeof f);
  if (jm_scan(&jm, probed, &f) < 1) result = -1;
  else if (uid && ((f.kind != JM_FOUND_OURS && f.kind != JM_FOUND_UPDATER) || memcmp(f.id.uid, b.uid, 16))) result = -2;
  else {
    jm_pad_init(&pad, &jm, &p.fw, &f);
    result = 0;
    if (backup && !jm_pad_backup(&pad, to_kotlin, &b)) result = -3;
    else if (action == 0 && !backup && !jm_pad_ready(&pad)) result = -1;
    else if (action == 1 && !jm_pad_install(&pad)) result = -4;
    else if (action == 3 && !jm_pad_reboot(&pad)) result = -4;
    else if (action == 2) {
      jbyte *data = (*e)->GetByteArrayElements(e, image, NULL);
      if (!data || !jm_pad_restore(&pad, (const uint8_t *)data, (size_t)(*e)->GetArrayLength(e, image), raw)) result = -4;
      if (data) (*e)->ReleaseByteArrayElements(e, image, data, JNI_ABORT);
    }
  }
  close_package(e, &p);
  return result;
}

/* A stock pad's USB identity is in the library's table. */
JNIEXPORT jboolean JNICALL Java_dev_jollymaam_app_NativeCore_isStock(JNIEnv *e, jobject self, jint vid, jint pid) {
  const struct jm_stock *s;
  (void)e, (void)self;
  for (size_t i = 0; (s = jm_stock_at(i)); i++)
    if (s->vid == vid && s->product == pid) return JNI_TRUE;
  return JNI_FALSE;
}
