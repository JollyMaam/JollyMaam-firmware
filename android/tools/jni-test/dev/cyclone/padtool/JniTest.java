package dev.cyclone.padtool;
import java.nio.file.*;
import java.util.*;
import java.io.*;
import java.util.zip.*;
public class JniTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static void put(byte[] data, int offset, int value) { for (int i=0;i<4;i++) data[offset+i]=(byte)(value >>> (8*i)); }
    static int get(byte[] data, int offset) { int value=0;for(int i=0;i<4;i++) value|=(data[offset+i]&255)<<(8*i);return value; }
    static class Custom extends NativeUsb {
        int cmd,tag,client,begun,written,reconnects,starts,progress,rom;boolean updater,committed,fail;
        byte[] flash = new byte[4096];
        ArrayDeque<byte[]> answers = new ArrayDeque<>();
        /* The answer as input reports: client, tag, state, u16 offset, 56 bytes. */
        void answer(byte[] body) {
            answers.clear();
            int at=0;
            do {
                byte[] r=new byte[64];int k=Math.min(56,body.length-at);
                put(r,0,client);r[4]=(byte)tag;r[5]=(byte)(at+k<body.length?2:3);r[6]=(byte)at;r[7]=(byte)(at>>8);
                System.arraycopy(body,at,r,8,k);answers.add(r);at+=k;
            } while(at<body.length);
        }
        public int hidRead(byte[] data,int timeout) {
            byte[] r=answers.poll();if(r==null) return 0;
            System.arraycopy(r,0,data,0,64);return 64;
        }
        public int featureSize() { return 356; }
        public int featureSet(byte[] data) {
            check(data.length==357 && (data[19]&2)!=0);cmd=data[2]&255;tag=data[20]&255;client=get(data,21);
            int arg=get(data,3),size=(data[17]&255)|((data[18]&255)<<8);
            if(cmd==0x11) rom++;
            else if(cmd==0x40) updater=true;
            else if(cmd==0x42) { check(size==44);begun=arg; }
            else if(cmd==0x43) {
                if(fail) throw new IllegalStateException("simulated callback exception");
                check(arg==written);System.arraycopy(data,25,flash,written,size);written+=size;
            } else if(cmd==0x44) { check(written==begun);committed=true; }
            else if(cmd==0x10) { check(committed);starts++; }
            byte[] body=new byte[0];
            if(cmd==0) {
                body=new byte[88];System.arraycopy("C2PD".getBytes(),0,body,0,4);
                body[7]=(byte)(updater?1:0);body[24]=42;
                System.arraycopy("U2C_WIRELESS".getBytes(),0,body,40,12);
            } else if(cmd==0x41) {
                body=new byte[32];body[0]=1;body[1]=(byte)(committed?2:begun>0?1:0);body[3]=(byte)(committed?1:0);
                put(body,8,4096);put(body,12,written);put(body,16,begun);put(body,24,128);
            }
            answer(body);
            return data.length;
        }
        public boolean reconnect(byte[] uid) { check(uid.length==16 && uid[0]==42);reconnects++;return true; }
        public void onProgress(long done,long total) { check(done<=total);progress++; }
    }
    public static void main(String[] args) throws Exception {
        byte[] bytes=Files.readAllBytes(Path.of(args[0]));NativeCore core=new NativeCore();
        String[] products=core.products(bytes);check(products.length==3 && products[0].equals("U2C_WIRELESS"));
        check(core.products(new byte[2])==null);
        check(core.products(null)==null && core.probe(null,0)==null);
        check(core.run(bytes,"U2C_WIRELESS",0,null,new Custom())==-100);
        check(core.run(bytes,"U2C_WIRELESS",0,new byte[15],new Custom())==-100);
        check(core.run(bytes,"U2C_WIRELESS",99,null,new Custom())==-100);
        Custom transport=new Custom();check(core.probe(transport,0).length==88);
        byte[] uid=new byte[16];uid[0]=42;
        check(core.run(bytes,"U2C_WIRELESS",0,uid,transport)==0);
        check(transport.starts==1 && transport.reconnects==1 && transport.progress>0);
        byte[] expected;
        try(ZipInputStream zip=new ZipInputStream(new ByteArrayInputStream(bytes))) {
            expected=null;ZipEntry entry;
            while((entry=zip.getNextEntry())!=null) if(entry.getName().equals("update.bin")) { expected=zip.readAllBytes();break; }
        }
        check(expected!=null && Arrays.equals(expected,Arrays.copyOf(transport.flash,transport.written)));
        Custom wrong=new Custom();uid[0]=43;
        check(core.run(bytes,"U2C_WIRELESS",0,uid,wrong)!=0 && wrong.begun==0);uid[0]=42;
        Custom entry=new Custom();entry.updater=true;
        check(core.stockLoader(uid,entry)==0 && entry.rom==1 && entry.begun==0);
        uid[0]=43;check(core.stockLoader(uid,new Custom())==-2);uid[0]=42;
        Custom broken=new Custom();broken.fail=true;
        try { core.run(bytes,"U2C_WIRELESS",0,uid,broken);throw new AssertionError(); }
        catch(IllegalStateException e) { check(e.getMessage().contains("callback exception")); }
        check(!broken.committed && broken.starts==0);
        byte[] inquiry=core.probe(new NativeUsb() {
            public int scsi(byte[] cdb,int direction,byte[] data,int timeout) {
                check(cdb.length==6 && cdb[0]==0x12 && direction==1 && data.length==36);
                System.arraycopy("BR23".getBytes(),0,data,8,4);System.arraycopy("UBOOT1.00".getBytes(),0,data,16,9);
                return data.length;
            }
        },2);
        check(new String(inquiry).equals("BR23"));
        byte[] model=core.probe(new NativeUsb() {
            public int hidWrite(byte[] data) { check(data.length==64 && data[0]==5 && data[1]==8);return data.length; }
            public int hidRead(byte[] data,int timeout) {
                data[1]=5;data[4]=8;data[6]=2;data[18]=0x1b;data[19]=0x30;return 64;
            }
        },1);
        check(model.length==2 && model[0]==0x1b);
        System.out.println("JNI package verification, USB callbacks, reconnect, progress and exception tests passed");
    }
}
