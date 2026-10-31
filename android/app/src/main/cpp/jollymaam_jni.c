#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "jollymaam.h"
struct bridge { JNIEnv *env;jobject transport;jmethodID scsi,write,read,set,get,wait,reconnect,size,progress; };
static int prepare(struct bridge *b,JNIEnv *env,jobject transport) {
  memset(b,0,sizeof *b);b->env=env;b->transport=transport;
  if(!transport) return 0;
  jclass cls=(*env)->GetObjectClass(env,transport);if(!cls) return 0;
  b->scsi=(*env)->GetMethodID(env,cls,"scsi","([BI[BI)I");
  b->write=(*env)->GetMethodID(env,cls,"hidWrite","([B)I");
  b->read=(*env)->GetMethodID(env,cls,"hidRead","([BI)I");
  b->set=(*env)->GetMethodID(env,cls,"featureSet","([B)I");
  b->get=(*env)->GetMethodID(env,cls,"featureGet","([B)I");
  b->wait=(*env)->GetMethodID(env,cls,"waitMillis","(I)V");
  b->reconnect=(*env)->GetMethodID(env,cls,"reconnect","([B)Z");
  b->size=(*env)->GetMethodID(env,cls,"featureSize","()I");
  b->progress=(*env)->GetMethodID(env,cls,"onProgress","(JJ)V");
  (*env)->DeleteLocalRef(env,cls);return !(*env)->ExceptionCheck(env);
}
static jbyteArray bytes(JNIEnv *env,const uint8_t *data,size_t size) {
  jbyteArray result=(*env)->NewByteArray(env,(jsize)size);
  if(result && size && data) (*env)->SetByteArrayRegion(env,result,0,(jsize)size,(const jbyte *)data);
  return result;
}
static int array_call(struct bridge *b,jmethodID method,uint8_t *data,size_t size,unsigned timeout,int timed) {
  JNIEnv *e=b->env;if((*e)->ExceptionCheck(e)) return -1;
  jbyteArray array=bytes(e,data,size);if(!array) return -1;
  jint n=timed?(*e)->CallIntMethod(e,b->transport,method,array,(jint)timeout):(*e)->CallIntMethod(e,b->transport,method,array);
  if(!(*e)->ExceptionCheck(e) && n>=0 && (size_t)n<=size) {
    if(method==b->read || method==b->get) (*e)->GetByteArrayRegion(e,array,0,n,(jbyte *)data);
  }
  else n=-1;
  (*e)->DeleteLocalRef(e,array);return n;
}
static int scsi(void *user,const uint8_t *cdb,size_t n,enum jm_scsi_direction direction,
                 uint8_t *data,size_t size,unsigned timeout,struct jm_scsi_result *result) {
  struct bridge *b=user;JNIEnv *e=b->env;if((*e)->ExceptionCheck(e)) return -1;
  jbyteArray command=bytes(e,cdb,n),array=bytes(e,direction==JM_SCSI_OUT?data:NULL,size);
  if(!command || !array) { if(command) (*e)->DeleteLocalRef(e,command);if(array) (*e)->DeleteLocalRef(e,array);return -1; }
  jint actual=(*e)->CallIntMethod(e,b->transport,b->scsi,command,(jint)direction,array,(jint)timeout);
  int ok=!(*e)->ExceptionCheck(e) && actual>=0 && (size_t)actual<=size;
  if(ok) {
    if(direction==JM_SCSI_IN && actual) (*e)->GetByteArrayRegion(e,array,0,actual,(jbyte *)data);
    memset(result,0,sizeof *result);result->transferred=(size_t)actual;
  }
  (*e)->DeleteLocalRef(e,command);(*e)->DeleteLocalRef(e,array);return ok?0:-1;
}
static int write_hid(void *u,const uint8_t *d,size_t n) { struct bridge *b=u;return array_call(b,b->write,(uint8_t *)d,n,0,0); }
static int read_hid(void *u,uint8_t *d,size_t n,unsigned t) { struct bridge *b=u;memset(d,0,n);return array_call(b,b->read,d,n,t,1); }
static int feature_set(void *u,const uint8_t *d,size_t n) { struct bridge *b=u;return array_call(b,b->set,(uint8_t *)d,n,0,0); }
static int feature_get(void *u,uint8_t *d,size_t n) { struct bridge *b=u;return array_call(b,b->get,d,n,0,0); }
static size_t feature_size(void *u) {
  struct bridge *b=u;JNIEnv *e=b->env;if((*e)->ExceptionCheck(e)) return 0;
  jint n=(*e)->CallIntMethod(e,b->transport,b->size);return n>0?(size_t)n:0;
}
static void wait_ms(void *u,unsigned n) {
  struct bridge *b=u;JNIEnv *e=b->env;if(!(*e)->ExceptionCheck(e)) (*e)->CallVoidMethod(e,b->transport,b->wait,(jint)n);
}
static int reconnect(void *u,const uint8_t uid[16]) {
  struct bridge *b=u;JNIEnv *e=b->env;if((*e)->ExceptionCheck(e)) return 0;
  jbyteArray array=bytes(e,uid,16);if(!array) return 0;
  jboolean ok=(*e)->CallBooleanMethod(e,b->transport,b->reconnect,array);(*e)->DeleteLocalRef(e,array);
  return !(*e)->ExceptionCheck(e) && ok;
}
static void progress(void *u,size_t done,size_t total) {
  struct bridge *b=u;JNIEnv *e=b->env;if(!(*e)->ExceptionCheck(e)) (*e)->CallVoidMethod(e,b->transport,b->progress,(jlong)done,(jlong)total);
}
static const struct jm_session_io io={scsi,write_hid,read_hid,feature_set,feature_get,wait_ms,reconnect,feature_size};
/* Verify the whole package before returning selectable product/board/chip triples.
 * Java owns the byte array; no native pointer escapes this call. */
JNIEXPORT jobjectArray JNICALL Java_dev_cyclone_padtool_NativeCore_products(JNIEnv *e,jobject self,jbyteArray input) {
  (void)self;if(!input || (*e)->GetArrayLength(e,input)>32*1024*1024) return NULL;
  jbyte *data=(*e)->GetByteArrayElements(e,input,NULL);if(!data) return NULL;
  struct jm_package package;struct jm_manifest manifest;jobjectArray result=NULL;
  if(jm_package_open(&package,(uint8_t *)data,(size_t)(*e)->GetArrayLength(e,input))==JM_PACKAGE_OK && jm_manifest_open(&manifest,&package)) {
    jclass string=(*e)->FindClass(e,"java/lang/String");
    if(string) {
      result=(*e)->NewObjectArray(e,(jsize)manifest.count*3,string,NULL);
      for(unsigned i=0;result && i<manifest.count && !(*e)->ExceptionCheck(e);++i) {
        const char *values[]={manifest.targets[i].product,manifest.targets[i].board,manifest.targets[i].chip};
        for(unsigned k=0;k<3;++k) {
          jstring value=(*e)->NewStringUTF(e,values[k]);if(!value) break;
          (*e)->SetObjectArrayElement(e,result,(jsize)(i*3+k),value);(*e)->DeleteLocalRef(e,value);
        }
      }
      (*e)->DeleteLocalRef(e,string);
    }
  }
  (*e)->ReleaseByteArrayElements(e,input,data,JNI_ABORT);return result;
}
/* Read-only protocol probing. Android retains endpoint ownership and permissions. */
JNIEXPORT jbyteArray JNICALL Java_dev_cyclone_padtool_NativeCore_probe(JNIEnv *e,jobject self,jobject transport,jint kind) {
  (void)self;struct bridge b;if(!prepare(&b,e,transport)) return NULL;
  uint8_t data[88]={0};size_t size=0;
  if(kind==JM_SESSION_CUSTOM) {
    struct jm_identity id;
    if(jm_session_identify(&io,&b,&id)) {
      memcpy(data,"C2PD",4);data[4]=(uint8_t)id.protocol;data[5]=(uint8_t)(id.protocol>>8);
      data[6]=id.board;data[7]=id.updater;memcpy(data+8,id.build,16);memcpy(data+24,id.uid,16);
      memcpy(data+40,id.product,16);memcpy(data+56,id.board_name,32);size=88;
    }
  } else if(kind==JM_SESSION_8BITDO) {
    uint8_t report[64],reply[128];jm_8bitdo_request(report,8,0,NULL,4);
    if(write_hid(&b,report,64)==64) {
      int n=read_hid(&b,reply,sizeof reply,1000);const uint8_t *payload;size_t length;
      if(n>0 && jm_8bitdo_reply(reply,(size_t)n,8,0,&payload,&length)==JM_8BITDO_OK && length==2) { memcpy(data,payload,2);size=2; }
    }
  } else if(kind==JM_SESSION_JIELI) {
    uint8_t cdb[6]={0x12,0,0,0,36,0};struct jm_scsi_result result;
    if(!scsi(&b,cdb,6,JM_SCSI_IN,data,36,2000,&result) && result.transferred==36) size=36;
  }
  return size && !(*e)->ExceptionCheck(e)?bytes(e,data,size):NULL;
}
/* Revalidate the package in the worker call, select its product, and run the
 * same core session as desktop. JNI callbacks are synchronous on this thread.
 * Java exceptions remain pending while core cleanup releases native buffers. */
JNIEXPORT jint JNICALL Java_dev_cyclone_padtool_NativeCore_run(JNIEnv *e,jobject self,jbyteArray input,
                                      jstring product,jint kind,jbyteArray uid,jobject transport) {
  (void)self;
  if(!input || !product || kind<JM_SESSION_CUSTOM || kind>JM_SESSION_JIELI ||
     (*e)->GetArrayLength(e,input)>32*1024*1024 ||
     (kind==JM_SESSION_CUSTOM && (!uid || (*e)->GetArrayLength(e,uid)!=16))) return -100;
  struct bridge b;if(!prepare(&b,e,transport)) return -100;
  jbyte *data=(*e)->GetByteArrayElements(e,input,NULL);if(!data) return -100;
  const char *name=(*e)->GetStringUTFChars(e,product,NULL);
  if(!name) { (*e)->ReleaseByteArrayElements(e,input,data,JNI_ABORT);return -100; }
  struct jm_package package;struct jm_manifest manifest;int result=-100;
  uint8_t identity[16]={0};
  if(uid && (*e)->GetArrayLength(e,uid)==16) (*e)->GetByteArrayRegion(e,uid,0,16,(jbyte *)identity);
  if(!(*e)->ExceptionCheck(e) && jm_package_open(&package,(uint8_t *)data,(size_t)(*e)->GetArrayLength(e,input))==JM_PACKAGE_OK && jm_manifest_open(&manifest,&package)) {
    const struct jm_firmware_target *target=jm_manifest_target(&manifest,name);
    if(target) result=jm_session_run(target,(enum jm_session_kind)kind,identity,&io,&b,progress,&b);
  }
  (*e)->ReleaseStringUTFChars(e,product,name);(*e)->ReleaseByteArrayElements(e,input,data,JNI_ABORT);return result;
}

/* Send a UID-bound stock-loader request from the application's USB worker. */
JNIEXPORT jint JNICALL Java_dev_cyclone_padtool_NativeCore_stockLoader(JNIEnv *e,jobject self,jbyteArray uid,jobject transport) {
  (void)self;
  if(!uid || (*e)->GetArrayLength(e,uid)!=16) return -1;
  struct bridge b;if(!prepare(&b,e,transport)) return -1;
  uint8_t expected[16];(*e)->GetByteArrayRegion(e,uid,0,16,(jbyte *)expected);
  if((*e)->ExceptionCheck(e)) return -1;
  return jm_session_stock_loader(&io,&b,expected);
}
