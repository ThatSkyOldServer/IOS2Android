package local.ios.host;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;
import java.io.File;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.KeyStore;
import java.security.cert.CertificateFactory;
import java.security.cert.CertPathValidator;
import java.security.cert.PKIXParameters;
import java.security.cert.TrustAnchor;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.Locale;
import java.util.TimeZone;
import java.util.GregorianCalendar;
import java.util.ArrayList;
import java.util.UUID;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import android.provider.Settings;
import java.text.BreakIterator;
import java.util.regex.Pattern;
import java.util.regex.Matcher;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.content.res.Configuration;
import android.util.DisplayMetrics;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ConcurrentHashMap;
import org.json.JSONObject;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

public final class HostActivity extends Activity {
    // DEVELOPMENT_BEGIN
    private native void configurePerformanceSampling(boolean enabled);
    private native void configureShaderDiagnostics(boolean enabled);
    private native void configureLabelDiagnostics(boolean enabled);
    private native void sampleCpu(boolean enabled);
    private native void dumpLabelDiagnostics();
    private void configureBusinessDiagnostics(Intent intent) {
        String requested=intent.getStringExtra("businessDiagnostics");
        Set<String> categories=new HashSet<>(Arrays.asList(requested==null ? new String[0] : requested.split(",")));
        boolean capture=categories.contains("capture");
        configureRenderDiagnostics(capture,compatibilityDirectory());
        if (!capture) configurePerformanceSampling(categories.contains("performance"));
        configureShaderDiagnostics(categories.contains("shader"));
        configureNetworkDiagnostics(categories.contains("network"));
        configureInterfaceDiagnostics(categories.contains("interfaces"));
        configureLabelDiagnostics(categories.contains("labels"));
        if (categories.contains("cpu")) {
            sampleCpu(true); mainHandler.postDelayed(()->sampleCpu(false),15000);
        } else sampleCpu(false);
        if (categories.contains("labels")) mainHandler.postDelayed(this::dumpGuestLabelLayouts,1000);
        Log.i("I2AGuest","[BUSINESS] diagnostics="+categories);
    }
    private void dumpGuestLabelLayouts() {
        dumpLabelDiagnostics();
        int reported=0;
        for (java.util.Map.Entry<Long,TextView> entry:guestLabels.entrySet()) {
            TextView label=entry.getValue();
            LabelUpdate state=(LabelUpdate)label.getTag();
            android.text.Layout layout=label.getLayout();
            android.graphics.Rect visible=new android.graphics.Rect();
            boolean intersects=label.getLocalVisibleRect(visible);
            String text=label.getText().toString().replace('\n',' ').replace('\r',' ');
            if (text.length()>96) text=text.substring(0,96);
            Log.i("I2AGuest","[DIAG-label-layout] token="+Long.toHexString(entry.getKey())+
                " frame="+label.getX()+","+label.getY()+","+label.getWidth()+","+label.getHeight()+
                " textHeight="+(layout==null ? -1 : layout.getHeight())+" lines="+(layout==null ? -1 : layout.getLineCount())+
                " padding="+label.getCompoundPaddingTop()+","+label.getCompoundPaddingBottom()+
                " spacing="+label.getLineSpacingExtra()+","+label.getLineSpacingMultiplier()+
                " clip="+label.getClipBounds()+" scroll="+label.getScrollX()+","+label.getScrollY()+
                " firstLine="+(layout==null || layout.getLineCount()==0 ? "?" :
                    layout.getLineLeft(0)+","+layout.getLineRight(0)+","+layout.getLineBaseline(0)+","+layout.getLineAscent(0)+","+layout.getLineDescent(0))+
                " size="+label.getTextSize()+" chars="+label.length()+
                " shown="+label.isShown()+" visibility="+label.getVisibility()+" alpha="+label.getAlpha()+" z="+label.getZ()+
                " parent="+guestLabelOverlay.getWidth()+","+guestLabelOverlay.getHeight()+
                " intersects="+intersects+" visible="+visible+
                " native="+(state==null ? "?" : java.util.Arrays.toString(state.metrics))+
                " options="+(state==null ? "?" : java.util.Arrays.toString(state.options))+" text="+text);
            if (++reported==128) break;
        }
        Log.i("I2AGuest","[DIAG-label-layout] reported="+reported+" total="+guestLabels.size()+" pending="+pendingLabels.size());
    }
    // DEVELOPMENT_END
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        // DEVELOPMENT_BEGIN
        if (libraryLoaded && intent.hasExtra("businessDiagnostics")) configureBusinessDiagnostics(intent);
        // DEVELOPMENT_END
        receiveNotificationIntent(intent);
    }
    private TextView guestStatus;
    private LaunchScreenView guestLaunch;
    private static final class LaunchScreenView extends android.view.View {
        final android.graphics.Paint paint=new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG | android.graphics.Paint.FILTER_BITMAP_FLAG);
        final android.graphics.Bitmap[] images; final android.graphics.RectF[] bounds; final double[][][] layout;
        final float[] opacity; final int[] colours,modes,scales; final boolean[] hidden;
        final android.text.StaticLayout[] labels; final String[] captions; final int[] textColours,alignments,lineCounts;
        LaunchScreenView(Context context) throws org.json.JSONException {
            super(context);
            JSONObject scene=new JSONObject(BuildProfile.LAUNCH_SCREEN); org.json.JSONArray nodes=scene.getJSONArray("views");
            setBackgroundColor(colour(scene.getJSONArray("background")));
            int count=nodes.length(); images=new android.graphics.Bitmap[count]; bounds=new android.graphics.RectF[count]; layout=new double[count][4][3];
            opacity=new float[count]; colours=new int[count]; modes=new int[count]; scales=new int[count]; hidden=new boolean[count];
            labels=new android.text.StaticLayout[count]; captions=new String[count]; textColours=new int[count]; alignments=new int[count]; lineCounts=new int[count];
            float density=getResources().getDisplayMetrics().density; int preferred=density>2 ? 3 : density>1 ? 2 : 1;
            android.graphics.BitmapFactory.Options decode=new android.graphics.BitmapFactory.Options(); decode.inScaled=false;
            for(int i=0;i<count;i++) {
                JSONObject node=nodes.getJSONObject(i); int parent=node.getInt("parent"); bounds[i]=new android.graphics.RectF();
                hidden[i]=node.optBoolean("hidden") || parent>=0 && hidden[parent]; opacity[i]=(float)node.optDouble("alpha",1)*(parent>=0 ? opacity[parent] : 1);
                colours[i]=node.isNull("background") ? 0 : colour(node.getJSONArray("background")); modes[i]=node.optInt("contentMode");
                captions[i]=node.optString("text",""); textColours[i]=node.optJSONArray("textColor")==null ? android.graphics.Color.BLACK : colour(node.getJSONArray("textColor"));
                alignments[i]=node.optInt("alignment"); lineCounts[i]=node.optInt("lines",1);
                for(int axis=0;axis<4;axis++) for(int value=0;value<3;value++) layout[i][axis][value]=node.getJSONArray("layout").getJSONArray(axis).getDouble(value);
                org.json.JSONArray candidates=node.getJSONArray("images");
                for(int desired:new int[]{preferred,preferred==3 ? 2 : 1,preferred==3 ? 1 : 3}) {
                    for(int item=0;item<candidates.length();item++) {
                        JSONObject candidate=candidates.getJSONObject(item); if(candidate.getInt("scale")!=desired) continue;
                        int resource=getResources().getIdentifier(candidate.getString("resource"),"drawable",context.getPackageName());
                        images[i]=resource==0 ? null : android.graphics.BitmapFactory.decodeResource(getResources(),resource,decode); scales[i]=desired;
                        if(images[i]==null) throw new IllegalStateException("Unable to decode the original launch image.");
                        break;
                    }
                    if(images[i]!=null) break;
                }
            }
            if(BuildProfile.DEVELOPMENT) android.util.Log.i("I2AGuest","Original launch screen loaded: "+scene.optString("name")+", views="+count);
        }
        static int colour(org.json.JSONArray rgba) throws org.json.JSONException {
            int[] channels=new int[4]; for(int i=0;i<4;i++) channels[i]=(int)Math.round(Math.max(0,Math.min(1,rgba.getDouble(i)))*255);
            return android.graphics.Color.argb(channels[3],channels[0],channels[1],channels[2]);
        }
        @Override protected void onSizeChanged(int width,int height,int oldWidth,int oldHeight) {
            float density=getResources().getDisplayMetrics().density;
            for(int i=0;i<bounds.length;i++) {
                float[] values=new float[4];
                for(int axis=0;axis<4;axis++) values[axis]=(float)(layout[i][axis][0]*width+layout[i][axis][1]*height+layout[i][axis][2]*density);
                bounds[i].set(values[0],values[1],values[0]+values[2],values[1]+values[3]);
                labels[i]=null;
                if(!captions[i].isEmpty() && values[2]>0){
                    android.text.TextPaint text=new android.text.TextPaint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                    text.setTextSize(17*density); text.setColor(textColours[i]);
                    android.text.Layout.Alignment alignment=alignments[i]==1 ? android.text.Layout.Alignment.ALIGN_CENTER : alignments[i]==2 ? android.text.Layout.Alignment.ALIGN_OPPOSITE : android.text.Layout.Alignment.ALIGN_NORMAL;
                    labels[i]=android.text.StaticLayout.Builder.obtain(captions[i],0,captions[i].length(),text,Math.max(1,Math.round(values[2])))
                            .setAlignment(alignment).setIncludePad(false).setMaxLines(lineCounts[i]>0 ? lineCounts[i] : Integer.MAX_VALUE).build();
                }
            }
            if(BuildProfile.DEVELOPMENT) android.util.Log.i("I2AGuest","Original launch layout: "+width+"x"+height);
        }
        @Override protected void onDraw(android.graphics.Canvas canvas) {
            super.onDraw(canvas); float density=getResources().getDisplayMetrics().density;
            for(int i=0;i<images.length;i++) {
                if(hidden[i] || opacity[i]<=0) continue;
                android.graphics.RectF box=bounds[i];
                if(i!=0 && android.graphics.Color.alpha(colours[i])!=0) { paint.setColor(colours[i]); paint.setAlpha(Math.round(opacity[i]*android.graphics.Color.alpha(colours[i]))); canvas.drawRect(box,paint); }
                if(labels[i]!=null && box.width()>0 && box.height()>0){
                    labels[i].getPaint().setAlpha(Math.round(opacity[i]*android.graphics.Color.alpha(textColours[i])));
                    int save=canvas.save(); canvas.clipRect(box); canvas.translate(box.left,box.top+(box.height()-labels[i].getHeight())/2); labels[i].draw(canvas); canvas.restoreToCount(save);
                }
                android.graphics.Bitmap bitmap=images[i]; if(bitmap==null || box.width()<=0 || box.height()<=0) continue;
                android.graphics.RectF target=new android.graphics.RectF(box);
                if(modes[i]==1 || modes[i]==2 || modes[i]>=4) {
                    float width=bitmap.getWidth(),height=bitmap.getHeight(),factor=modes[i]>=4 ? density/scales[i] : modes[i]==1 ? Math.min(box.width()/width,box.height()/height) : Math.max(box.width()/width,box.height()/height);
                    width*=factor; height*=factor;
                    int mode=modes[i]; float x=mode==7 || mode==9 || mode==11 ? box.left : mode==8 || mode==10 || mode==12 ? box.right-width : box.centerX()-width/2;
                    float y=mode==5 || mode==9 || mode==10 ? box.top : mode==6 || mode==11 || mode==12 ? box.bottom-height : box.centerY()-height/2;
                    target.set(x,y,x+width,y+height);
                }
                paint.setAlpha(Math.round(opacity[i]*255)); int save=canvas.save(); canvas.clipRect(box); canvas.drawBitmap(bitmap,null,target,paint); canvas.restoreToCount(save);
            }
        }
        void release() { java.util.Arrays.fill(images,null); java.util.Arrays.fill(labels,null); }
    }
    private MotionSource motion;
    private native String deliverDeviceMotion(long token, int generation, double[] values);
    private boolean startDeviceMotion(long token, int generation, double interval) { return motion.start(token, generation, interval); }
    private void stopDeviceMotion(long token) { if (motion != null) motion.stop(token); }
    private android.widget.FrameLayout guestContent;
    private LabelOverlay guestLabelOverlay;
    private TableContent tableContent;
    private final java.util.HashMap<Long,ModalPresenter.ControllerPanel> controllerPanels=new java.util.HashMap<>();
    private native String guestControllerEvent(long token,int event,int width,int height);
    private native boolean guestControllerCanDismiss(long token);
    private void showGuestController(long token,int style,float width,float height,float[] anchor,int arrows,boolean animated){mainHandler.post(()->{
        if(isDestroyed()){guestControllerEvent(token,4,0,0);return;}
        try{ModalPresenter.ControllerPanel panel=new ModalPresenter.ControllerPanel(this,style,width,height,anchor,arrows,new ModalPresenter.ControllerCallback(){
            public void event(int event,int width,int height){String failure=guestControllerEvent(token,event,width,height);if(failure!=null)guestFailure(failure);if(event==3 || event==4)controllerPanels.remove(token);}
            public boolean canDismiss(){return guestControllerCanDismiss(token);}
        });controllerPanels.put(token,panel);panel.show(animated);}
        catch(RuntimeException error){ModalPresenter.ControllerPanel failed=controllerPanels.remove(token);if(failed!=null)failed.close();else guestControllerEvent(token,4,0,0);guestFailure("Unable to present this controller: "+error.getMessage());}
    });}
    private void dismissGuestController(long token,boolean animated){mainHandler.post(()->{ModalPresenter.ControllerPanel panel=controllerPanels.get(token);if(panel!=null)panel.dismiss(animated);else guestControllerEvent(token,3,0,0);});}
    private void resizeGuestController(long token,float width,float height,float dim){mainHandler.post(()->{ModalPresenter.ControllerPanel panel=controllerPanels.get(token);if(panel!=null)try{panel.resize(width,height,dim);}catch(RuntimeException error){panel.close();guestFailure("Unable to resize this controller: "+error.getMessage());}});}
    private native long[] bindGuestTableRow(long table,int position,long previous,float width);
    private native String guestTableEvent(long table,int event,int position,long row,float top,float width,int state);
    private TableContent tableContent() {
        if(tableContent==null)tableContent=new TableContent(this,new TableContent.Callback(){
            public long[] bind(long table,int position,long previous,float width){return bindGuestTableRow(table,position,previous,width);}
            public String event(long table,int event,int position,long row,float top,float width,int state){return guestTableEvent(table,event,position,row,top,width,state);}
            public void alpha(long token,android.view.View view,float value){applyGuestAlpha(token,view,value);}
            public void detached(long token){forgetGuestComposition(token);}
            public void failure(String message){guestFailure(message);}
        });
        return tableContent;
    }
    private void syncGuestTable(long token,long owner,long background,float[] metrics,int[] options){mainHandler.post(()->{LabelOverlay parent=guestOverlay(owner);if(!isDestroyed() && parent!=null)tableContent().sync(token,parent,background,metrics,options);});}
    private void syncGuestTableRow(long table,long row,float[] metrics,int[] options){mainHandler.post(()->{if(tableContent!=null)tableContent.appearance(table,row,metrics,options);});}
    private void commandGuestTable(long token,int action,int position,float offset,boolean animated,int mode){mainHandler.post(()->{if(tableContent!=null && !isDestroyed())tableContent.command(token,action,position,offset,animated,mode);});}
    private LabelOverlay guestOverlay(long owner) { if(owner==0)return guestLabelOverlay;ModalPresenter.ControllerPanel panel=controllerPanels.get(owner);return panel!=null ? panel.overlay() : tableContent==null ? null : tableContent.overlay(owner); }
    private final ConcurrentHashMap<Long, TextView> guestLabels = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Long, android.widget.ImageView> guestControls = new ConcurrentHashMap<>();
    private ViewComposition viewComposition;
    private native String completeViewAnimation(long token,int event,boolean completed);
    private android.view.View guestView(long token) {
        android.view.View view=guestLabels.get(token);if(view==null)view=guestControls.get(token);if(view==null && webContent!=null)view=webContent.view(token);if(view==null && textEditors!=null)view=textEditors.view(token);if(view==null && tableContent!=null)view=tableContent.view(token);if(view==null && viewComposition!=null)view=viewComposition.view(token);return view;
    }
    private ViewComposition viewComposition() {
        if(viewComposition==null)viewComposition=new ViewComposition(this,new ViewComposition.Callback(){
            public LabelOverlay overlay(long owner){return guestOverlay(owner);}
            public android.view.View view(long token){return guestView(token);}
            public String event(long token,int event,boolean completed){return completeViewAnimation(token,event,completed);}
            public void failure(String message){guestFailure(message);}
        });return viewComposition;
    }
    private void forgetGuestComposition(long token){if(viewComposition!=null)viewComposition.remove(token);}
    private void applyGuestAlpha(long token,android.view.View view,float value){viewComposition().alpha(token,view,value);}
    private static final class CompositionUpdate {
        final long owner;final boolean raw;final float[] presentation,clips;final long[] clipOwners;
        CompositionUpdate(long owner,boolean raw,float[] presentation,float[] clips,long[] clipOwners){this.owner=owner;this.raw=raw;this.presentation=presentation;this.clips=clips;this.clipOwners=clipOwners;}
    }
    private final ConcurrentHashMap<Long,CompositionUpdate> pendingCompositions=new ConcurrentHashMap<>();
    private void syncGuestComposition(long token,long owner,boolean raw,float[] presentation,float[] clips,long[] clipOwners) {
        if(pendingCompositions.put(token,new CompositionUpdate(owner,raw,presentation,clips,clipOwners))!=null)return;
        mainHandler.post(()->{CompositionUpdate update=pendingCompositions.remove(token);if(update!=null && !isDestroyed())viewComposition().sync(token,update.owner,update.raw,update.presentation,update.clips,update.clipOwners);});
    }
    private static final class LayerUpdate {
        final long owner;final boolean raw,replace,replacePattern;final float[] metrics,locations;final int[] options,colors;final byte[] image,pattern;
        LayerUpdate(long owner,boolean raw,boolean replace,float[] metrics,int[] options,int[] colors,float[] locations,byte[] image,boolean replacePattern,byte[] pattern){this.owner=owner;this.raw=raw;this.replace=replace;this.metrics=metrics;this.options=options;this.colors=colors;this.locations=locations;this.image=image;this.replacePattern=replacePattern;this.pattern=pattern;}
    }
    private final ConcurrentHashMap<Long,LayerUpdate> pendingLayers=new ConcurrentHashMap<>();
    private void syncGuestLayer(long token,long owner,boolean raw,boolean replace,float[] metrics,int[] options,int[] colors,float[] locations,byte[] image,boolean replacePattern,byte[] pattern) {
        final boolean[] schedule={false};pendingLayers.compute(token,(key,previous)->{schedule[0]=previous==null;return new LayerUpdate(owner,raw,replace || previous!=null && previous.replace,metrics,options,colors,locations,!replace && previous!=null ? previous.image : image,replacePattern || previous!=null && previous.replacePattern,!replacePattern && previous!=null ? previous.pattern : pattern);});
        if(schedule[0])mainHandler.post(()->{LayerUpdate update=pendingLayers.remove(token);if(update!=null && !isDestroyed())try{viewComposition().layer(token,update.owner,update.raw,update.replace,update.metrics,update.options,update.colors,update.locations,update.image,update.replacePattern,update.pattern);}catch(RuntimeException error){guestFailure("Unable to display the supplied layer: "+error.getMessage());}});
    }
    private void animateGuestViews(long[] changed,float[] before,float[] after,long[] tokens,int[] offsets,long[] ancestors,float[] model,float[] framesBefore,float[] framesAfter,long duration,long delay,int options,float damping,float velocity,float repeat,long completion) {
        long requested=android.os.SystemClock.uptimeMillis();mainHandler.post(()->{if(isDestroyed() || startupFailed){if(completion!=0)completeViewAnimation(completion,1,false);return;}viewComposition().animate(changed,before,after,tokens,offsets,ancestors,model,framesBefore,framesAfter,duration,delay,options,damping,velocity,repeat,completion,requested);});
    }
    private void reorderGuestLabels(long[] order) {
        mainHandler.post(()-> {
            if(guestLabelOverlay==null || isDestroyed()) return;
            for(long token:order) { android.view.View view=guestView(token); if(view!=null && view.getParent() instanceof LabelOverlay)((LabelOverlay)view.getParent()).bringChildToFront(view); }
        });
    }
    private final TextRenderer guestText = new TextRenderer(this);
    private static final class GuestLabel extends TextView {
        private CharSequence source;private float originalSize,minimumScale,factor=1,originalHeight,originalBaseline,naturalWidth;private boolean adjusts;private int baseline;
        GuestLabel(android.content.Context context){super(context);}
        void configure(CharSequence text,float size,boolean adjusts,float minimumScale,int baseline){
            boolean changed=source!=text || originalSize!=size;boolean settings=this.adjusts!=adjusts || this.minimumScale!=minimumScale || this.baseline!=baseline;source=text;originalSize=size;this.adjusts=adjusts;this.minimumScale=minimumScale;this.baseline=baseline;
            if(changed){float[] metrics=TextRenderer.singleLineMetrics(text,size);naturalWidth=metrics[0];originalHeight=metrics[1];originalBaseline=metrics[2];}if(changed || settings)fit(true);
        }
        private void fit(boolean changed){if(source==null)return;float available=Math.max(0,getWidth()-getPaddingLeft()-getPaddingRight());float scale=adjusts && available>0 && naturalWidth>available ? Math.max(Math.max(.001f,minimumScale),available/naturalWidth) : 1;if(changed || factor!=scale){factor=scale;super.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX,originalSize*scale);super.setText(TextRenderer.scaled(source,scale));invalidate();}}
        @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);if(w!=oldw)fit(false);}
        @Override protected void onDraw(android.graphics.Canvas canvas){android.text.Layout layout=getLayout();float shift=0;if(factor<1 && layout!=null){float originalTop=(getHeight()-getPaddingTop()-getPaddingBottom()-originalHeight)/2f,scaledTop=(getHeight()-getPaddingTop()-getPaddingBottom()-layout.getHeight())/2f;if(baseline==0)shift=originalTop+originalBaseline-scaledTop-layout.getLineBaseline(0);else if(baseline==2)shift=originalTop-scaledTop;}
            if(shift==0){super.onDraw(canvas);return;}int saved=canvas.save();canvas.translate(0,shift);super.onDraw(canvas);canvas.restoreToCount(saved);}
    }
    private android.text.SpannableStringBuilder createGuestText(String text) { return new android.text.SpannableStringBuilder(text); }
    private float addGuestTextRun(android.text.SpannableStringBuilder text,int start,int end,String name,int traits,float size,int color,int alignment,float spacing,float kern,float baseline) { return guestText.run(text,start,end,name,traits,size,color,alignment,spacing,kern,baseline); }
    private void addGuestTextAttachment(android.text.SpannableStringBuilder text,int index,byte[] png,float[] bounds,boolean template) { guestText.attachment(text,index,png,bounds,template); }
    private void addGuestTextLink(android.text.SpannableStringBuilder text,int start,int end,String url) { TextRenderer.link(text,start,end,url); }
    private static final class LabelUpdate {
        final long owner; final CharSequence text; final float[] metrics; final int[] options;
        LabelUpdate(long owner,CharSequence text, float[] metrics, int[] options) { this.owner=owner;this.text=text; this.metrics=metrics; this.options=options; }
    }
    private final ConcurrentHashMap<Long, LabelUpdate> pendingLabels = new ConcurrentHashMap<>();
    private void syncGuestLabel(long token,long owner, CharSequence text, float[] m, int[] o) {

        if (pendingLabels.put(token, new LabelUpdate(owner,text, m, o)) != null) return;
        mainHandler.post(() -> {
            LabelUpdate update=pendingLabels.remove(token);
            if (update!=null) applyGuestLabel(token,update.owner,update.text,update.metrics,update.options);
        });
    }
    private void applyGuestLabel(long token,long owner, CharSequence text, float[] m, int[] o) {
            if (guestContent == null || isDestroyed()) return;
            TextView label = guestLabels.get(token);
            if (o[7] == 0) {
                if (label != null) { forgetGuestComposition(token); LabelOverlay.detach(label); guestLabels.remove(token); }
                return;
            }
            LabelOverlay overlay=guestOverlay(owner);if(overlay==null)return;
            if (label == null) {
                if (o[4] == 0) return;
                label = new GuestLabel(this); label.setPadding(0, 0, 0, 0); label.setIncludeFontPadding(false);
                label.setFallbackLineSpacing(true);
                label.setBreakStrategy(android.text.Layout.BREAK_STRATEGY_SIMPLE); label.setHyphenationFrequency(android.text.Layout.HYPHENATION_FREQUENCY_NONE);
                guestLabels.put(token, label);
            }
            LabelOverlay.attach(overlay,label);
            LabelUpdate previous = (LabelUpdate)label.getTag();
            float[] p = previous == null ? null : previous.metrics;
            int[] q = previous == null ? null : previous.options;
            if (q == null || q[0] != o[0]) label.setTextColor(o[0]);
            if (q == null || q[2] != o[2]) label.setMaxLines(o[2] == 0 ? Integer.MAX_VALUE : o[2]);
            if (q == null || q[2] != o[2] || q[8] != o[8] || q[5]!=o[5]) {
                label.setHorizontallyScrolling(o[2]==1 && (o[8]>=2 || o[5]!=0));
                label.setEllipsize(o[8]==3 ? android.text.TextUtils.TruncateAt.START : o[8]==4 ? android.text.TextUtils.TruncateAt.END : o[8]==5 ? android.text.TextUtils.TruncateAt.MIDDLE : null);
            }
            if (q == null || q[1] != o[1]) label.setGravity(android.view.Gravity.CENTER_VERTICAL | (o[1] == 1 ? android.view.Gravity.CENTER_HORIZONTAL : o[1] == 2 ? android.view.Gravity.RIGHT : android.view.Gravity.LEFT));
            if (p == null || p[6] != m[6] || p[7] != m[7] || p[8] != m[8] || q[6] != o[6]) label.setShadowLayer(m[6], m[7], m[8], o[6]);
            applyGuestAlpha(token,label,m[5]); overlay.depth(label,m[10],m[11]);
            if (q == null || q[3] != o[3] || p[9] != m[9]) {
                android.graphics.drawable.GradientDrawable background = new android.graphics.drawable.GradientDrawable();
                background.setColor(o[3]); background.setCornerRadius(m[9]); label.setBackground(background);
            }

            if (p == null || p[3] != m[3] || p[4] != m[4]) label.setLayoutParams(new android.widget.FrameLayout.LayoutParams(Math.max(0, (int)Math.ceil(m[3])), Math.max(0, (int)Math.ceil(m[4]))));
            label.setTranslationX(m[1]); label.setTranslationY(m[2]);
            label.setClipBounds(new android.graphics.Rect((int)m[12],(int)m[13],(int)Math.ceil(m[12]+m[14]),(int)Math.ceil(m[13]+m[15])));
            label.setVisibility(o[4] != 0 ? android.view.View.VISIBLE : android.view.View.GONE);
            overlay.visibility(label,o[4] != 0 && m[5] > 0);
            ((GuestLabel)label).configure(text,m[0],o[5]!=0 && o[2]==1,m[16],o[9]);
            label.setTag(new LabelUpdate(owner,text,m,o));
            viewComposition().bind(token,label);
    }
    private float[] measureGuestText(CharSequence text, float size, float limit, int lines, int lineBreak) { return TextRenderer.measure(text,size,limit,lines,lineBreak); }
    private float[] measureGuestAttributedText(CharSequence text,float size,float width,float height,int options){return TextRenderer.measureAttributed(text,size,width,height,options);}
    private float guestFontScale() { return getResources().getConfiguration().fontScale; }
    private int[] guestInterfaceColors(){ModalPresenter.Palette palette=new ModalPresenter.Palette(this);return new int[]{palette.surface,palette.foreground,palette.muted,palette.accent,palette.field};}
    private int guestLayoutDirection() { return getResources().getConfiguration().getLayoutDirection(); }
    private native String completeControlEvent(long token, int event, boolean highlighted);
    private static final class ControlUpdate {
        final long owner; final byte[] image; final boolean replace; final float[] metrics; final int[] options; final String description;
        ControlUpdate(long owner,byte[] image,boolean replace,float[] metrics,int[] options,String description) { this.owner=owner;this.image=image; this.replace=replace; this.metrics=metrics; this.options=options; this.description=description; }
    }
    private final ConcurrentHashMap<Long, ControlUpdate> pendingControls = new ConcurrentHashMap<>();
    private final Set<Long> pressedControls = new HashSet<>();
    private long exclusiveControl;
    private void syncGuestControl(long token,long owner, byte[] image, boolean replace, float[] metrics, int[] options, String description) {
        final boolean[] schedule={false};
        pendingControls.compute(token,(key,previous)-> {
            schedule[0]=previous==null;
            return new ControlUpdate(owner,!replace && previous!=null ? previous.image : image,replace || previous!=null && previous.replace,metrics,options,description);
        });
        if (schedule[0]) mainHandler.post(()-> {
            ControlUpdate update=pendingControls.remove(token);
            if(update!=null) applyGuestControl(token,update);
        });
    }
    private void applyGuestControl(long token, ControlUpdate update) {
        if(guestContent==null || isDestroyed()) return;
        float[] m=update.metrics; int[] o=update.options;
        android.widget.ImageView view=guestControls.get(token);
        if(o[1]==0) {
            if(view!=null) { forgetGuestComposition(token); if(view.getDrawable() instanceof android.graphics.drawable.Animatable) ((android.graphics.drawable.Animatable)view.getDrawable()).stop(); LabelOverlay.detach(view); guestControls.remove(token); view.setImageDrawable(null); }
            pressedControls.remove(token); if(exclusiveControl==token) exclusiveControl=0; return;
        }
        LabelOverlay overlay=guestOverlay(update.owner);if(overlay==null)return;
        if(view==null) {
            if(o[0]==0) return;
            view=new android.widget.ImageView(this); guestControls.put(token,view);
            if(o[8]!=0) view.setImageDrawable(new android.widget.ProgressBar(this).getIndeterminateDrawable().mutate());
            final android.widget.ImageView control=view; final boolean[] inside={false};
            view.setOnTouchListener((widget,event)-> {
                ControlUpdate state=(ControlUpdate)control.getTag();
                if(state==null || state.options[2]==0 || state.options[3]==0) return false;
                int action=event.getActionMasked(),code=0; boolean highlighted=false;
                boolean contains=event.getX()>=0 && event.getY()>=0 && event.getX()<control.getWidth() && event.getY()<control.getHeight();
                if(action==android.view.MotionEvent.ACTION_DOWN) {
                    if(exclusiveControl!=0 && exclusiveControl!=token || state.options[6]!=0 && !pressedControls.isEmpty()) return true;
                    pressedControls.add(token); if(state.options[6]!=0) exclusiveControl=token;
                    inside[0]=true; highlighted=true; code=1;
                } else {
                    if(!pressedControls.contains(token)) return true;
                    if(action==android.view.MotionEvent.ACTION_MOVE) {
                        code=contains ? 4 : 8; if(contains!=inside[0]) code|=contains ? 16 : 32;
                        inside[0]=contains; highlighted=contains;
                    } else if(action==android.view.MotionEvent.ACTION_UP || action==android.view.MotionEvent.ACTION_CANCEL) {
                        code=action==android.view.MotionEvent.ACTION_CANCEL ? 256 : contains ? 64 : 128;
                        pressedControls.remove(token); if(exclusiveControl==token) exclusiveControl=0;
                    } else return true;
                }
                String failure=completeControlEvent(token,code,highlighted); if(failure!=null) guestFailure(failure); return true;
            });
        }
        LabelOverlay.attach(overlay,view);
        ControlUpdate old=(ControlUpdate)view.getTag(); float[] p=old==null ? null : old.metrics; int[] q=old==null ? null : old.options;
        if(update.replace && o[8]==0) {
            android.graphics.Bitmap bitmap=update.image==null ? null : android.graphics.BitmapFactory.decodeByteArray(update.image,0,update.image.length);
            if(update.image!=null && bitmap==null) guestFailure("Unable to decode the supplied UIKit image.");
            view.setImageBitmap(bitmap);
        }
        if(o[0]==0 || o[3]==0) { pressedControls.remove(token); if(exclusiveControl==token) exclusiveControl=0; }
        view.setScaleType(o[8]!=0 || o[5]==1 ? android.widget.ImageView.ScaleType.FIT_CENTER : o[5]==2 ? android.widget.ImageView.ScaleType.CENTER_CROP : o[5]>=4 ? android.widget.ImageView.ScaleType.MATRIX : android.widget.ImageView.ScaleType.FIT_XY);
        android.graphics.drawable.Drawable image=view.getDrawable();
        if(o[8]==0 && o[5]>=4 && image!=null && image.getIntrinsicWidth()>0 && image.getIntrinsicHeight()>0) {
            int mode=o[5]; float x=mode==7 || mode==9 || mode==11 ? 0 : mode==8 || mode==10 || mode==12 ? m[2]-m[7] : (m[2]-m[7])/2;
            float y=mode==5 || mode==9 || mode==10 ? 0 : mode==6 || mode==11 || mode==12 ? m[3]-m[8] : (m[3]-m[8])/2;
            android.graphics.Matrix matrix=new android.graphics.Matrix(); matrix.setScale(m[7]/image.getIntrinsicWidth(),m[8]/image.getIntrinsicHeight()); matrix.postTranslate(x,y); view.setImageMatrix(matrix);
        }
        view.setEnabled(o[3]!=0); view.setClickable(o[2]!=0); view.setContentDescription(update.description);
        view.setImportantForAccessibility(update.description==null && o[7]==0 ? android.view.View.IMPORTANT_FOR_ACCESSIBILITY_NO : android.view.View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        if(p==null || p[2]!=m[2] || p[3]!=m[3]) view.setLayoutParams(new android.widget.FrameLayout.LayoutParams(Math.max(0,(int)Math.ceil(m[2])),Math.max(0,(int)Math.ceil(m[3]))));
        if(q==null || q[4]!=o[4] || p[5]!=m[5]) {
            android.graphics.drawable.GradientDrawable background=new android.graphics.drawable.GradientDrawable(); background.setColor(o[4]); background.setCornerRadius(m[5]); view.setBackground(background);
        }
        view.setTranslationX(m[0]); view.setTranslationY(m[1]); applyGuestAlpha(token,view,m[4]); overlay.depth(view,m[6],m[9]);
        view.setClipBounds(new android.graphics.Rect((int)m[10],(int)m[11],(int)Math.ceil(m[10]+m[12]),(int)Math.ceil(m[11]+m[13])));
        view.setVisibility(o[0]!=0 ? android.view.View.VISIBLE : android.view.View.GONE); overlay.visibility(view,o[0]!=0 && m[4]>0);
        if(o[8]!=0 && image!=null) {
            if(q==null || q[10]!=o[10]) image.setTint(o[10]);
            if(image instanceof android.graphics.drawable.Animatable) {
                android.graphics.drawable.Animatable animation=(android.graphics.drawable.Animatable)image;
                if(o[9]!=0 && o[0]!=0 && m[4]>0) { if(!animation.isRunning()) animation.start(); }
                else animation.stop();
            }
        }
        view.setTag(new ControlUpdate(update.owner,null,false,m,o,update.description));
        viewComposition().bind(token,view);
    }
    private boolean guestScreenRequested;
    private final java.util.concurrent.atomic.AtomicInteger processDisplayRequests=new java.util.concurrent.atomic.AtomicInteger();
    private void applyGuestScreenPolicy() {
        if(isDestroyed())return;
        if(guestScreenRequested || processDisplayRequests.get()>0)getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
    private void guestKeepScreenOn(boolean enabled) { mainHandler.post(()->{guestScreenRequested=enabled;applyGuestScreenPolicy();}); }
    private int[] guestSafeInsets() {
        android.view.WindowInsets insets = getWindow().getDecorView().getRootWindowInsets();
        if (insets == null) return new int[4];
        int top, left, bottom, right;
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets padding = insets.getInsets(android.view.WindowInsets.Type.systemBars() | android.view.WindowInsets.Type.displayCutout());
            top = padding.top; left = padding.left; bottom = padding.bottom; right = padding.right;
        } else {
            top = insets.getSystemWindowInsetTop(); left = insets.getSystemWindowInsetLeft(); bottom = insets.getSystemWindowInsetBottom(); right = insets.getSystemWindowInsetRight();
            android.view.DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) { top = Math.max(top, cutout.getSafeInsetTop()); left = Math.max(left, cutout.getSafeInsetLeft()); bottom = Math.max(bottom, cutout.getSafeInsetBottom()); right = Math.max(right, cutout.getSafeInsetRight()); }
        }
        return new int[]{top, left, bottom, right};
    }
    private boolean guestOpenUrl(String value, boolean query) {
        try {
            android.net.Uri uri=android.net.Uri.parse(value);
            Intent intent = "app-settings".equalsIgnoreCase(uri.getScheme())
                ? new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,android.net.Uri.fromParts("package",getPackageName(),null))
                : new Intent(Intent.ACTION_VIEW,uri);
            if (query) return getPackageManager().resolveActivity(intent, 0) != null;
            startActivity(intent); return true;
        } catch (android.content.ActivityNotFoundException | SecurityException | IllegalArgumentException failure) { return false; }
    }
    private android.view.accessibility.AccessibilityManager guestAccessibility;
    private android.view.accessibility.AccessibilityManager.AccessibilityStateChangeListener accessibilityListener;
    private android.view.accessibility.AccessibilityManager.TouchExplorationStateChangeListener explorationListener;
    private Object servicesListener;
    private native void updateGuestAccessibility(boolean spoken);
    private void refreshGuestAccessibility() {
        if(guestAccessibility!=null && libraryLoaded && !isDestroyed()) updateGuestAccessibility(guestAccessibility.isEnabled() && !guestAccessibility.getEnabledAccessibilityServiceList(android.accessibilityservice.AccessibilityServiceInfo.FEEDBACK_SPOKEN).isEmpty());
    }
    private static final class AccessibilityApi33 {
        static Object attach(HostActivity host,android.view.accessibility.AccessibilityManager manager) {
            android.view.accessibility.AccessibilityManager.AccessibilityServicesStateChangeListener listener=value->host.refreshGuestAccessibility();
            manager.addAccessibilityServicesStateChangeListener(host.getMainExecutor(),listener);return listener;
        }
        static void detach(android.view.accessibility.AccessibilityManager manager,Object listener) { manager.removeAccessibilityServicesStateChangeListener((android.view.accessibility.AccessibilityManager.AccessibilityServicesStateChangeListener)listener); }
    }
    private void openGuestAccessibility() {
        guestAccessibility=(android.view.accessibility.AccessibilityManager)getSystemService(ACCESSIBILITY_SERVICE);
        if(guestAccessibility==null)return;
        accessibilityListener=value->refreshGuestAccessibility();explorationListener=value->refreshGuestAccessibility();
        guestAccessibility.addAccessibilityStateChangeListener(accessibilityListener);guestAccessibility.addTouchExplorationStateChangeListener(explorationListener);
        if(android.os.Build.VERSION.SDK_INT>=33)servicesListener=AccessibilityApi33.attach(this,guestAccessibility);
        refreshGuestAccessibility();
    }
    private void closeGuestAccessibility() {
        if(guestAccessibility==null)return;
        guestAccessibility.removeAccessibilityStateChangeListener(accessibilityListener);guestAccessibility.removeTouchExplorationStateChangeListener(explorationListener);
        if(android.os.Build.VERSION.SDK_INT>=33 && servicesListener!=null)AccessibilityApi33.detach(guestAccessibility,servicesListener);
        guestAccessibility=null;servicesListener=null;
    }
    private void postGuestAccessibility(int notification,long token,String text) {
        mainHandler.post(()->{
            if(isDestroyed() || guestAccessibility==null || !guestAccessibility.isEnabled())return;
            android.view.View view=guestLabels.get(token);if(view==null)view=guestControls.get(token);if(view==null)view=guestContent;if(view==null)return;
            if(text!=null) { android.view.accessibility.AccessibilityEvent event=android.view.accessibility.AccessibilityEvent.obtain(android.view.accessibility.AccessibilityEvent.TYPE_ANNOUNCEMENT);event.setPackageName(getPackageName());event.setClassName(view.getClass().getName());event.setSource(view);event.getText().add(text);guestAccessibility.sendAccessibilityEvent(event); }
            else view.sendAccessibilityEvent(notification==1000 ? android.view.accessibility.AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED : android.view.accessibility.AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
            if(token!=0)view.performAccessibilityAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS,null);
        });
    }
    private String guestClipboardText() {
        android.content.ClipboardManager clipboard = (android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
        android.content.ClipData data = clipboard.getPrimaryClip();
        if (data == null || data.getItemCount() == 0) return null;
        CharSequence text = data.getItemAt(0).getText(); return text == null ? null : text.toString();
    }
    private void guestClipboardText(String text) {
        android.content.ClipboardManager clipboard = (android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
        if (text == null) clipboard.clearPrimaryClip();
        else clipboard.setPrimaryClip(android.content.ClipData.newPlainText("I2A", text));
    }
    private ShareSheet shares;
    private native String completeGuestShare(long token, int result);
    private void showGuestShare(long token, String text, byte[][] data, String[] paths, String[] exclusions) {
        mainHandler.post(() -> {
            if (shares==null) shares=new ShareSheet(this,mainHandler,operationWorkers,(request,result)-> {
                String failure=completeGuestShare(request,result); if(failure!=null) guestFailure(failure);
            });
            shares.show(token,text,data,paths,exclusions);
        });
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if(photos!=null && photos.result(request,result,data))return;
        if (shares!=null) shares.result(request,result);
    }
    private void dismissGuestShare(long token, long completion) {
        mainHandler.post(() -> {
            if (shares!=null) shares.dismiss(token);
            String failure = completeModalDismiss(completion);
            if (failure != null) guestFailure(failure);
        });
    }
    private boolean libraryLoaded, guestStarted;
    private android.widget.EditText guestInput;
    private TextInput guestIme;
    private TextInput.Editors textEditors;
    private KeyboardState guestKeyboard;
    private boolean guestKeyboardReady;
    private long guestInputToken;
    private boolean guestInputSync;
    private native String editGuestInput(long token, String value, boolean submit);
    private native boolean editorFocus(long token,boolean focused);
    private native void editorSelection(long token,int start,int end);
    private native void editorScroll(long token,float x,float y,float width,float height);
    private TextInput.Editors textEditors(){
        if(textEditors==null)textEditors=new TextInput.Editors(this,guestLabelOverlay,new TextInput.Editors.Callback(){
            public String edit(long token,String text){return editGuestInput(token,text,false);}
            public boolean focus(long token,boolean focused){return editorFocus(token,focused);}
            public void selection(long token,int start,int end){editorSelection(token,start,end);}
            public void submit(long token){editGuestInput(token,"",true);}
            public void scroll(long token,float x,float y,float width,float height){editorScroll(token,x,y,width,height);}
            public void alpha(long token,android.view.View view,float value){applyGuestAlpha(token,view,value);}
            public void detached(long token){forgetGuestComposition(token);}
        });
        return textEditors;
    }
    private void syncGuestEditor(long token,long owner,CharSequence text,String hint,float[] metrics,int[] options){mainHandler.post(()->{LabelOverlay overlay=guestOverlay(owner);if(!isDestroyed() && overlay!=null)textEditors().sync(token,overlay,text,hint,metrics,options);});}
    private void focusGuestEditor(long token,boolean show){mainHandler.post(()->{if(!isDestroyed() && textEditors!=null)textEditors.focus(token,show);});}
    private void closeGuestEditor(long token){mainHandler.post(()->{if(textEditors!=null)textEditors.remove(token);});}
    private native String updateGuestKeyboard(boolean visible, int x, int y, int width, int height);
    private float[] guestFontMetrics(String name, int traits, float size) { return guestText.fontMetrics(name, traits, size); }
    private byte[] renderGuestImage(int width, int height, double scale, boolean opaque, double[] commands, byte[][] images) { return TextRenderer.renderImage(width,height,(float)scale,opaque,commands,images); }
    private byte[] convertGuestImage(byte[] image, boolean jpeg, int quality) { return TextRenderer.convertImage(image,jpeg,quality); }
    private byte[] guestFontBytes(String name) { return guestText.fontBytes(name); }
    private void syncGuestInput(long token, String text, int keyboard, int returnKey, int capitals, boolean secure, boolean showKeyboard) {
        mainHandler.post(() -> {
            if (guestInput == null) {
                guestInput = new android.widget.EditText(this);
                guestInput.setSingleLine(true); guestInput.setAlpha(0); guestInput.setPadding(0, 0, 0, 0);
                guestIme = new TextInput(guestInput);
                guestContent.addView(guestInput, new android.widget.FrameLayout.LayoutParams(1, 1));
                guestInput.addTextChangedListener(new android.text.TextWatcher() {
                    public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                    public void onTextChanged(CharSequence s, int start, int before, int count) {}
                    public void afterTextChanged(android.text.Editable text) {
                        if (guestInputSync || guestInputToken == 0) return;
                        String accepted = editGuestInput(guestInputToken, text.toString(), false);
                        if (accepted != null && !accepted.contentEquals(text)) {
                            guestInputSync = true; guestInput.setText(accepted); guestInput.setSelection(accepted.length()); guestInputSync = false;
                        }
                    }
                });
                guestInput.setOnEditorActionListener((v, action, event) -> {
                    if (guestInputToken != 0) editGuestInput(guestInputToken, guestInput.getText().toString(), true);
                    return true;
                });
            }
            guestInputSync = true; guestInputToken = token;
            int type = TextInput.inputType(keyboard, capitals, secure), options = TextInput.options(returnKey);
            boolean restart = guestInput.getInputType() != type || guestInput.getImeOptions() != options;
            if (guestInput.getInputType() != type) guestInput.setInputType(type);
            if (guestInput.getImeOptions() != options) guestInput.setImeOptions(options);
            if (!guestInput.getText().toString().equals(text)) { guestInput.setText(text); guestInput.setSelection(text.length()); }
            guestInputSync = false;
            if (restart && guestInput.hasFocus()) ((android.view.inputmethod.InputMethodManager)getSystemService(INPUT_METHOD_SERVICE)).restartInput(guestInput);
            if (showKeyboard) guestIme.requestShow();
        });
    }
    private void hideGuestInput(long token) {
        mainHandler.post(() -> {
            if(textEditors!=null && textEditors.focus(token,false))return;
            if (guestInput == null || guestInputToken != token) return;
            guestInputToken = 0;
            guestIme.cancel();
            ((android.view.inputmethod.InputMethodManager)getSystemService(INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(guestInput.getWindowToken(), 0);
            guestInput.clearFocus(); guestContent.setFocusableInTouchMode(true); guestContent.requestFocus();
        });
    }
    private native String deliverTouches(int action, int changed, long timestamp, int[] ids, float[] positions);
    private double[] gestureConfiguration() {
        android.view.ViewConfiguration configuration = android.view.ViewConfiguration.get(this);
        return new double[]{configuration.getScaledTouchSlop()/getResources().getDisplayMetrics().density,
            android.view.ViewConfiguration.getDoubleTapTimeout()/1000.0, android.view.ViewConfiguration.getLongPressTimeout()/1000.0};
    }
    private boolean guestTouch(android.view.MotionEvent event) {
        if (!guestLoaded || startupFailed) return true;
        int action = event.getActionMasked();
        if (action != 0 && action != 1 && action != 2 && action != 3 && action != 5 && action != 6) return false;
        int count = event.getPointerCount();
        int[] ids = new int[count]; float[] positions = new float[count * 4];
        for (int i = 0; i < count; i++) ids[i] = event.getPointerId(i);
         
        if (action == android.view.MotionEvent.ACTION_MOVE) for (int h = 0; h < event.getHistorySize(); h++) {
            for (int i = 0; i < count; i++) {
                positions[i * 4] = event.getHistoricalX(i, h); positions[i * 4 + 1] = event.getHistoricalY(i, h);
                positions[i * 4 + 2] = event.getHistoricalPressure(i, h); positions[i * 4 + 3] = event.getToolType(i);
            }
            String failure = deliverTouches(action, -1, event.getHistoricalEventTime(h), ids, positions);
            if (failure != null) { guestFailure(failure); return true; }
        }
        for (int i = 0; i < count; i++) {
            positions[i * 4] = event.getX(i); positions[i * 4 + 1] = event.getY(i);
            positions[i * 4 + 2] = event.getPressure(i); positions[i * 4 + 3] = event.getToolType(i);
        }
        String failure = deliverTouches(action, event.getPointerId(event.getActionIndex()), event.getEventTime(), ids, positions);
        if (failure != null) guestFailure(failure);
        return true;
    }
    private void cancelGuestTouches() {
        if (!guestLoaded || startupFailed) return;
        String failure = deliverTouches(3, -1, android.os.SystemClock.uptimeMillis(), new int[0], new float[0]);
        if (failure != null) guestFailure(failure);
    }
    private FrameScheduler frameScheduler;
    private native void updateStatusBarSnapshot(long style, boolean hidden, double height);
    private long guestStatusBarStyle = -1, lastStatusBarStyle = -1;
    private boolean lastStatusBarHidden;
    private double lastStatusBarHeight = -1;
    private void updateGuestStatusBarSnapshot(android.view.WindowInsets insets) {
        if (!libraryLoaded || insets == null) return;
        android.view.View decor = getWindow().getDecorView();
        boolean dark, hidden;
        int height;
        if (Build.VERSION.SDK_INT >= 30) {
            android.view.WindowInsetsController controller = decor.getWindowInsetsController();
            if (controller == null) return;
            dark = (controller.getSystemBarsAppearance() & android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS) != 0;
            hidden = !insets.isVisible(android.view.WindowInsets.Type.statusBars());
            height = hidden ? 0 : insets.getInsets(android.view.WindowInsets.Type.statusBars()).top;
        } else {
            int flags = decor.getSystemUiVisibility();
            dark = (flags & android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR) != 0;
            hidden = (flags & android.view.View.SYSTEM_UI_FLAG_FULLSCREEN) != 0;
            height = hidden ? 0 : insets.getSystemWindowInsetTop();
        }
        long style = guestStatusBarStyle;
        if (style < 0 || dark != (style == 0 || style == 3)) style = dark ? 0 : 1;
        double points = height / (double)getResources().getDisplayMetrics().density;
        if (style == lastStatusBarStyle && hidden == lastStatusBarHidden && points == lastStatusBarHeight) return;
        lastStatusBarStyle = style; lastStatusBarHidden = hidden; lastStatusBarHeight = points;
        updateStatusBarSnapshot(style, hidden, points);
    }
    private void guestStatusBarStyle(long style) {
        runOnUiThread(() -> {
            guestStatusBarStyle = style;
            android.view.View decor = getWindow().getDecorView();
            boolean dark = style == 0 || style == 3;
            if (Build.VERSION.SDK_INT >= 30) {
                android.view.WindowInsetsController controller = decor.getWindowInsetsController();
                if (controller != null) controller.setSystemBarsAppearance(dark ? android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS : 0,
                        android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS);
            } else {
                int flags = decor.getSystemUiVisibility();
                decor.setSystemUiVisibility(dark ? flags | android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR : flags & ~android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
            }
            updateGuestStatusBarSnapshot(decor.getRootWindowInsets());
        });
    }
    private void guestStatusBarHidden(boolean hidden) {
        runOnUiThread(() -> {
            android.view.View decor = getWindow().getDecorView();
            if (Build.VERSION.SDK_INT >= 30) {
                android.view.WindowInsetsController controller = decor.getWindowInsetsController();
                if (controller != null) {
                    if (hidden) controller.hide(android.view.WindowInsets.Type.statusBars());
                    else controller.show(android.view.WindowInsets.Type.statusBars());
                }
            } else {
                int flags = decor.getSystemUiVisibility();
                decor.setSystemUiVisibility(hidden ? flags | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN : flags & ~android.view.View.SYSTEM_UI_FLAG_FULLSCREEN);
            }
        });
    }
    private native void setRenderSurface(android.view.Surface surface, int width, int height);
    // DEVELOPMENT_BEGIN
    private native void configureRenderDiagnostics(boolean enabled, String directory);
    private native void configureNetworkDiagnostics(boolean enabled);
    private native void configureInterfaceDiagnostics(boolean enabled);
    // DEVELOPMENT_END
    private void setDisplayLinksActive(boolean active,float rate) { frameScheduler.active(active,rate); }
    private int motionSensorMask() {
        android.hardware.SensorManager manager = (android.hardware.SensorManager)getSystemService(SENSOR_SERVICE);
        if (manager == null) return 0;
        int mask = 0;
        if (manager.getDefaultSensor(android.hardware.Sensor.TYPE_ACCELEROMETER) != null) mask |= 1;
        if (manager.getDefaultSensor(android.hardware.Sensor.TYPE_GYROSCOPE) != null) mask |= 2;
        if (manager.getDefaultSensor(android.hardware.Sensor.TYPE_MAGNETIC_FIELD) != null) mask |= 4;
        if (motion != null && motion.available()) mask |= 8;
        return mask;
    }
    private native String startGuest(String filesDirectory);
    static native int configureCrashLog(String path);
    private native void updateSystemSnapshot(int width, int height, float scale, int rotation, boolean tablet, float battery, int batteryState, boolean lowPower);
    private native String updateApplicationState(int state);
    private native String expireBackgroundTask(long identifier);
    private boolean guestLoaded;
    private boolean startupFailed;
    private void updateBadge(int count) {
        NotificationManager manager = (NotificationManager)getSystemService(NOTIFICATION_SERVICE);
        if (manager == null) return;
        for (android.service.notification.StatusBarNotification entry : manager.getActiveNotifications()) {
            android.app.Notification update = android.app.Notification.Builder.recoverBuilder(this, entry.getNotification())
                .setNumber(count).setBadgeIconType(count == 0 ? android.app.Notification.BADGE_ICON_NONE : android.app.Notification.BADGE_ICON_SMALL).build();
            manager.notify(entry.getTag(), entry.getId(), update);
        }
    }
    private void firstFrameRendered() {
        mainHandler.post(() -> {
            if(startupFailed) return;
            if(guestLaunch!=null) { guestContent.removeView(guestLaunch); guestLaunch.release(); guestLaunch=null; }
            guestStatus.setVisibility(android.view.View.GONE);
        });
    }
    private boolean guestWindowDrawPending;
    private void guestWindowReady(){
        mainHandler.post(()->{
            if(startupFailed || guestLaunch==null || guestWindowDrawPending)return;
            guestWindowDrawPending=true;
            android.view.ViewTreeObserver observer=guestContent.getViewTreeObserver();
            observer.addOnDrawListener(new android.view.ViewTreeObserver.OnDrawListener(){
                boolean delivered;
                @Override public void onDraw(){if(delivered)return;delivered=true;guestContent.post(()->{if(observer.isAlive())observer.removeOnDrawListener(this);Log.i("I2AGuest","Guest UIWindow reached its first Android draw");firstFrameRendered();});}
            });
            guestContent.invalidate();
        });
    }
    private byte[] hmac(String algorithm, byte[] key, byte[] input) throws java.security.GeneralSecurityException {
        javax.crypto.Mac mac = javax.crypto.Mac.getInstance(algorithm);
         
        mac.init(new javax.crypto.spec.SecretKeySpec(key.length == 0 ? new byte[]{0} : key, algorithm));
        return mac.doFinal(input);
    }
    private byte[] crypt(int operation,int algorithm,int options,byte[] key,byte[] iv,byte[] input,int[] status) {
        status[0]=0;
        String name=algorithm==0 ? "AES" : algorithm==1 ? "DES" : algorithm==2 ? "DESede" : algorithm==4 ? "ARC4" : "Blowfish";
        String transformation=algorithm==4 ? name : name+((options&2)!=0 ? "/ECB/" : "/CBC/")+((options&1)!=0 ? "PKCS5Padding" : "NoPadding");
        try {
            javax.crypto.Cipher cipher=javax.crypto.Cipher.getInstance(transformation);
            javax.crypto.spec.SecretKeySpec secret=new javax.crypto.spec.SecretKeySpec(key,name);
            int mode=operation==0 ? javax.crypto.Cipher.ENCRYPT_MODE : javax.crypto.Cipher.DECRYPT_MODE;
            if (iv==null) cipher.init(mode,secret);
            else cipher.init(mode,secret,new javax.crypto.spec.IvParameterSpec(iv));
            return cipher.doFinal(input);
        } catch (javax.crypto.BadPaddingException failure) { status[0]=-4304; }
        catch (javax.crypto.IllegalBlockSizeException failure) { status[0]=-4303; }
        catch (java.security.NoSuchAlgorithmException | javax.crypto.NoSuchPaddingException failure) { status[0]=-4305; }
        catch (java.security.InvalidKeyException | java.security.InvalidAlgorithmParameterException | IllegalArgumentException failure) { status[0]=-4300; }
        catch (OutOfMemoryError failure) { status[0]=-4302; }
        finally { Arrays.fill(key,(byte)0); }
        return null;
    }
    private String transformText(String text,String identifier,boolean reverse) {
        try {
            return android.icu.text.Transliterator.getInstance(identifier,reverse ? android.icu.text.Transliterator.REVERSE : android.icu.text.Transliterator.FORWARD).transliterate(text);
        } catch (IllegalArgumentException failure) { return null; }
    }
    private String textLanguage(String text) {
        if (Build.VERSION.SDK_INT<29 || text.isEmpty()) return null;
        android.view.textclassifier.TextClassificationManager manager=getSystemService(android.view.textclassifier.TextClassificationManager.class);
        if (manager==null) return null;
        android.view.textclassifier.TextLanguage result=manager.getTextClassifier().detectLanguage(new android.view.textclassifier.TextLanguage.Request.Builder(text).build());
        if (result.getLocaleHypothesisCount()==0) return null;
        android.icu.util.ULocale locale=result.getLocale(0);
        return result.getConfidenceScore(locale)>=0.5f && !locale.getLanguage().equals("und") ? locale.getLanguage() : null;
    }
    private String timezoneAbbreviation(String identifier,long milliseconds,int fixedSeconds) {
        TimeZone zone=fixedSeconds==Integer.MIN_VALUE ? TimeZone.getTimeZone(identifier) : new java.util.SimpleTimeZone(fixedSeconds*1000,identifier);
        return zone.getDisplayName(zone.inDaylightTime(new java.util.Date(milliseconds)),TimeZone.SHORT,Locale.US);
    }
    private String audioDecoderName(String mime,int policy) {
        String fallback=null;
        for (android.media.MediaCodecInfo codec:new android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (codec.isEncoder()) continue;
            boolean supports=false;
            for (String type:codec.getSupportedTypes()) if (type.equalsIgnoreCase(mime)) { supports=true; break; }
            if (!supports) continue;
            boolean software=Build.VERSION.SDK_INT>=29 ? codec.isSoftwareOnly() : codec.getName().startsWith("OMX.google.");
            boolean hardware=Build.VERSION.SDK_INT>=29 ? codec.isHardwareAccelerated() : !software;
            if (policy==1 && !software || policy==2 && !hardware) continue;
            if (fallback==null) fallback=codec.getName();
            if (policy==1 || policy==2 || policy==3 && software || policy==4 && hardware) return codec.getName();
        }
        return fallback;
    }
    private native void networkChanged(int flags);
    private ConnectivityManager.NetworkCallback networkMonitor;
    private final BroadcastReceiver batteryMonitor = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) { if (guestLoaded) updateSystemSnapshot(); }
    };
    private int activityState = 2;
    private native String runOperation(long token);
    private final ExecutorService operationWorkers = Executors.newCachedThreadPool();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private HelpshiftBridge helpshift;
    private WebContent webContent;
    private native long webViewEvent(long token,int event,long operation,String data);
    private WebContent webContent() {
        if(webContent==null)webContent=new WebContent(this,guestLabelOverlay,new WebContent.Callback() {
            @Override public long event(long token,int kind,long operation,String data) { return webViewEvent(token,kind,operation,data); }
            @Override public void alpha(long token,android.view.View view,float value) { applyGuestAlpha(token,view,value); }
            @Override public void detached(long token) { forgetGuestComposition(token); }
        });
        return webContent;
    }
    private void webCommand(long token,String action,String payload) { mainHandler.post(()-> { if(!isDestroyed() && guestLabelOverlay!=null)webContent().command(token,action,payload); }); }
    private void syncGuestWeb(long token,long owner,float[] metrics,int[] options) { mainHandler.post(()-> { LabelOverlay overlay=guestOverlay(owner);if(!isDestroyed() && overlay!=null)webContent().sync(token,overlay,metrics,options); }); }
    private native void deliverHelpshiftEvent(String event,String data);
    private void helpshiftCommand(String operation,String data) {
        mainHandler.post(()-> {
            if (isDestroyed() || isFinishing()) return;
            try {
                if (helpshift==null) helpshift=new HelpshiftBridge(this,mainHandler,this::deliverHelpshiftEvent);
                helpshift.command(operation,data);
            } catch (Exception failure) { Log.w("I2AHost","Helpshift bridge initialization failed"); }
        });
    }
    private final HttpTransport http = new HttpTransport(mainHandler, operationWorkers, this::guestFailure);
    private final AudioOutput sound = new AudioOutput(mainHandler, this::guestFailure);
    private AudioSession audioSession;
    private PhotoLibrary photos;
    private native String completePhotoPicker(long token,int result,String metadata,byte[] bytes);
    private native String completePhotoRead(long token,byte[] bytes,String metadata,int error);
    private native String completePhotoAuthorization(long token,int result);
    private boolean photoSourceAvailable(int source){return photos.sourceAvailable(source);}
    private int photoAuthorization(int access){return photos.authorization(access);}
    private void authorizePhotos(long token,int access){photos.authorize(token,access);}
    private void showGuestPhotoPicker(long token){photos.pick(token);}
    private void dismissGuestPhotoPicker(long token,long completion){photos.dismiss(token);mainHandler.post(()->{String failure=completeModalDismiss(completion);if(failure!=null)guestFailure(failure);});}
    private String[] fetchPhotoAssets(String[] identifiers,int limit){return photos.assets(identifiers,limit);}
    private Object[] readPhotoData(String uri,int[] options){return photos.read(uri,options);}
    private void requestPhotoData(long token,String uri,int[] options){photos.request(token,uri,options);}
    private void cancelPhotoRequest(long token){photos.cancel(token);}
    private native String completePhotoWrite(long token, long code, String description);
    private void saveGuestPhoto(long token, byte[] bytes) { photos.save(token, bytes); }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (photos != null) photos.permissionResult(request, results);
        if (locations != null) locations.permissionResult(request);
        if (request == NOTIFICATION_PERMISSION) completeNotificationPermissions();
    }
    private CredentialStore keychain;
    private byte[] keychainRead() throws java.security.GeneralSecurityException, IOException { return keychain.read(); }
    private void keychainWrite(byte[] data) throws java.security.GeneralSecurityException, IOException { keychain.write(data); }
    private boolean keychainUnlocked() { return keychain.unlocked(); }
    private boolean keychainAfterFirstUnlock() { return keychain.afterFirstUnlock(); }
    private byte[][] generateRsaPair(int bits) throws java.security.GeneralSecurityException { return RsaKeys.generatePair(bits); }
    private java.security.Key importRsaKey(byte[] data, boolean privateKey) throws java.security.GeneralSecurityException { return RsaKeys.importKey(data, privateKey); }
    private int rsaKeyBits(java.security.Key key) { return RsaKeys.bitLength(key); }
    private byte[] rsaSign(java.security.Key key, int padding, byte[] data) throws java.security.GeneralSecurityException { return RsaKeys.sign(key, padding, data); }
    private boolean rsaVerify(java.security.Key key, int padding, byte[] data, byte[] signature) throws java.security.GeneralSecurityException { return RsaKeys.verify(key, padding, data, signature); }
    private int audioSessionConfigure(int property, double value) { return audioSession.configure(property, value); }
    private double audioSessionProperty(int property) { return audioSession.property(property); }
    private String[][] audioSessionRoute() { return audioSession.route(); }
    private boolean prepareAudio(long token, byte[] data) { return sound.prepare(token, data); }
    private String audioError() { return sound.lastError(); }
    private boolean playAudio(long token) { return sound.play(token); }
    private void pauseAudio(long token) { sound.pause(token); }
    private void stopAudio(long token) { sound.stop(token); }
    private boolean disposeAudio(long token) { return sound.dispose(token); }
    private void systemSoundFeedback() { mainHandler.post(() -> getWindow().getDecorView().performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)); }
    private void audioVolume(long token, float volume) { sound.volume(token, volume); }
    private double audioProperty(long token, int property) { return sound.property(token, property); }
    private boolean audioSeek(long token,double seconds) { return sound.seek(token,seconds); }
    private boolean audioLoops(long token,int loops) { return sound.loops(token,loops); }
    private void startConnection(long token) { http.start(token); }
    private void sendConnectionSynchronously(long token) { http.sendSynchronously(token); }
    private void cancelConnection(long token) { http.cancel(token); }
    private void resolveConnectionAuthentication(long token, int disposition) { http.resolveAuthentication(token, disposition); }
    private void resolveConnectionDecision(long token, int disposition) { http.resolveDecision(token, disposition); }
    private void suspendConnection(long token, boolean suspended) { http.suspend(token, suspended); }
    private String cookieCommand(long token,String operation,String json) throws Exception { return http.cookieCommand(token,operation,json); }
    private native String completeAnimation(long block,boolean completed);
    private native String completeAlert(long alert, int action);
    private native String editAlertField(long alert, int index, String value);
    private native String completeModalDismiss(long block);
    private final ConcurrentHashMap<Long, GuestModal> customGuestModals = new ConcurrentHashMap<>();
    private GuestModal createGuestModal(String presentation,CharSequence title,CharSequence message,CharSequence[] actions,float scale,byte[] image,java.util.function.IntConsumer select) {
        return null;
    }
    private void showGuestModal(long token,String presentation,CharSequence title,CharSequence message,CharSequence[] actions,float scale,byte[] image,boolean animated) {
        mainHandler.post(() -> {
            if (isFinishing() || isDestroyed()) return;
            GuestModal dialog=createGuestModal(presentation,title,message,actions,scale,image,action -> {
                if (!customGuestModals.containsKey(token)) return;
                String failure=completeAlert(token,action);
                if (failure!=null) guestFailure(failure);
            });
            if (dialog==null) { guestFailure("Unavailable modal presentation: "+presentation); return; }
            customGuestModals.put(token,dialog);
            dialog.show(animated,() -> {
                if(customGuestModals.get(token)!=dialog)return;
                String failure=completeAlert(token,-1);
                if (failure!=null) guestFailure(failure);
            });
        });
    }
    private final ConcurrentHashMap<Long, ModalPresenter> guestAlerts = new ConcurrentHashMap<>();
    private void selectGuestAlert(long token, ModalPresenter dialog, int action) {
        if (!guestAlerts.remove(token,dialog)) return;
        String failure=completeAlert(token,action);
        dialog.dismiss();
        if (failure!=null) guestFailure(failure);
        failure=completeAlert(token,-3);
        if (failure!=null) guestFailure(failure);
    }
    private void showGuestAlert(long token, int style, int cancel, String title, String message, String[] actions, boolean[] enabled, int[] styles, String[] values, String[] hints, int[] options) {
        mainHandler.post(() -> {
            if (isFinishing() || isDestroyed()) return;
            ModalPresenter dialog = new ModalPresenter(this,style,cancel,title,message,actions,enabled,styles,values,hints,options,
                (index,value) -> editAlertField(token,index,value),
                action -> {
                    ModalPresenter current=guestAlerts.get(token);
                    if (current!=null) selectGuestAlert(token,current,action);
                });
            guestAlerts.put(token,dialog); dialog.show();
            String failure=completeAlert(token,-1);
            if (failure!=null) guestFailure(failure);
        });
    }
    private void setGuestAlertActionEnabled(long token, int index, boolean enabled) {
        mainHandler.post(() -> {
            GuestModal custom=customGuestModals.get(token);
            if (custom!=null) { custom.actionEnabled(index,enabled); return; }
            ModalPresenter dialog=guestAlerts.get(token);
            if (dialog!=null) dialog.actionEnabled(index,enabled);
        });
    }
    private void dismissGuestAlert(long token, long completion,boolean animated) {
        mainHandler.post(() -> {
            GuestModal custom=customGuestModals.remove(token);
            if (custom!=null) { custom.dismiss(animated,() -> finishGuestModalDismiss(token,completion)); return; }
            ModalPresenter dialog=guestAlerts.remove(token);
            if (dialog!=null) dialog.dismiss();
            finishGuestModalDismiss(token,completion);
        });
    }
    private void finishGuestModalDismiss(long token,long completion) {
        String dismissed=completeAlert(token,-3);
        if (dismissed!=null) guestFailure(dismissed);
        String failure=completeModalDismiss(completion);
        if (failure!=null) guestFailure(failure);
    }
    private void guestHaptic(int kind) {
        mainHandler.post(() -> getWindow().getDecorView().performHapticFeedback(
            kind == 2 ? android.view.HapticFeedbackConstants.LONG_PRESS : android.view.HapticFeedbackConstants.KEYBOARD_TAP));
    }
    private native String runGuestWork(long token, int generation);
    private final ConcurrentHashMap<Long, Runnable> scheduledGuestWork = new ConcurrentHashMap<>();
    private synchronized void scheduleGuestWork(long token, int generation, long milliseconds) {
        Runnable previous = scheduledGuestWork.remove(token);
        if (previous != null) mainHandler.removeCallbacks(previous);
        Runnable action = new Runnable() {
            @Override public void run() {
                if (!scheduledGuestWork.remove(token, this)) return;
                String failure = runGuestWork(token, generation);
                if (failure != null) guestFailure(failure);
            }
        };
        scheduledGuestWork.put(token, action);
        mainHandler.postDelayed(action, milliseconds);
    }
    private final ConcurrentHashMap<Long, BackgroundTask> backgroundTasks = new ConcurrentHashMap<>();
    private static final class ProcessActivity {
        final long options;final PowerManager.WakeLock wake;
        ProcessActivity(long options,PowerManager.WakeLock wake){this.options=options;this.wake=wake;}
    }
    private final ConcurrentHashMap<Long,ProcessActivity> processActivities=new ConcurrentHashMap<>();
    private boolean processActivitiesClosed;
    private synchronized boolean processActivity(long identifier,long options,boolean begin) {
        if(!begin){ProcessActivity activity=processActivities.remove(identifier);if(activity==null)return true;
            if(activity.wake!=null && activity.wake.isHeld())activity.wake.release();
            if((activity.options&(1L<<40))!=0){processDisplayRequests.decrementAndGet();mainHandler.post(this::applyGuestScreenPolicy);}return true;
        }
        if(processActivitiesClosed || isDestroyed() || isFinishing())return false;
        PowerManager.WakeLock wake=null;
        try {
            if((options&(1L<<20))!=0){PowerManager manager=(PowerManager)getSystemService(POWER_SERVICE);wake=manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"I2A:process-activity");wake.setReferenceCounted(false);wake.acquire();}
            ProcessActivity activity=new ProcessActivity(options,wake);
            if(processActivities.putIfAbsent(identifier,activity)!=null){if(wake!=null && wake.isHeld())wake.release();return true;}
            if((options&(1L<<40))!=0){processDisplayRequests.incrementAndGet();mainHandler.post(this::applyGuestScreenPolicy);}
            return true;
        }catch(RuntimeException unavailable){if(wake!=null && wake.isHeld())wake.release();return false;}
    }
    private static final class BackgroundTask {
        final PowerManager.WakeLock wakeLock;
        final Runnable expiration;
        BackgroundTask(PowerManager.WakeLock wakeLock, Runnable expiration) { this.wakeLock = wakeLock; this.expiration = expiration; }
    }

    private boolean beginBackgroundTask(long identifier) {
        PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
        PowerManager.WakeLock wake = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "I2A:guest-background");
        wake.setReferenceCounted(false);
        Runnable expiration = () -> {
            if (!backgroundTasks.containsKey(identifier)) return;
            String failure = expireBackgroundTask(identifier);
            if (failure != null) guestFailure(failure);
        };
        BackgroundTask task = new BackgroundTask(wake, expiration);
        backgroundTasks.put(identifier, task);
        if (activityState == 2 && !armBackgroundTask(task)) { backgroundTasks.remove(identifier); return false; }
        return true;
    }

    private boolean armBackgroundTask(BackgroundTask task) {
        try { task.wakeLock.acquire(31_000L); }
        catch (SecurityException denied) { return false; }
        mainHandler.removeCallbacks(task.expiration);
        mainHandler.postDelayed(task.expiration, 30_000L);
        return true;
    }

    private void endBackgroundTask(long identifier) {
        BackgroundTask task = backgroundTasks.remove(identifier);
        if (task == null) return;
        mainHandler.removeCallbacks(task.expiration);
        if (task.wakeLock.isHeld()) task.wakeLock.release();
    }

    private void guestFailure(String message) {
        CrashLog.record(message, null);
        Log.e("I2AGuest", message);
        runOnUiThread(() -> { startupFailed = true; if(guestLaunch!=null) { guestContent.removeView(guestLaunch); guestLaunch.release(); guestLaunch=null; } guestStatus.setVisibility(android.view.View.VISIBLE); guestStatus.setText(message); });
    }

    private void updateGuestApplicationState() {
        if (!guestLoaded) return;
        String failure = updateApplicationState(activityState);
        if (failure != null) guestFailure(failure);
    }

    private void updateSystemSnapshot() {
        updateGuestApplicationState();
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        float level = -1;
        int state = 0;
        if (battery != null) {
            int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
            int charge = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            if (scale > 0 && charge >= 0) level = (float) charge / scale;
            int status = battery.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN);
            if (status == BatteryManager.BATTERY_STATUS_CHARGING) state = 2;
            else if (status == BatteryManager.BATTERY_STATUS_FULL) state = 3;
            else if (status == BatteryManager.BATTERY_STATUS_DISCHARGING || status == BatteryManager.BATTERY_STATUS_NOT_CHARGING) state = 1;
        }
        PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
        updateThermalSnapshot(guestThermalStatus);
        updateSystemSnapshot(metrics.widthPixels, metrics.heightPixels, metrics.density,
            getWindowManager().getDefaultDisplay().getRotation(), getResources().getConfiguration().smallestScreenWidthDp >= 600,
            level, state, power != null && power.isPowerSaveMode());
        updateGuestStatusBarSnapshot(getWindow().getDecorView().getRootWindowInsets());
    }
    private String systemProxySettings() {
        try {
            ConnectivityManager manager=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);
            android.net.ProxyInfo proxy=manager.getDefaultProxy();
            JSONObject result=new JSONObject();
            result.put("HTTPEnable",0); result.put("HTTPSEnable",0); result.put("SOCKSEnable",0);
            if(proxy==null) return result.toString();
            android.net.Uri pac=proxy.getPacFileUrl();
            if(pac!=null && !android.net.Uri.EMPTY.equals(pac)) {
                result.put("ProxyAutoConfigEnable",1); result.put("ProxyAutoConfigURLString",pac.toString());
            } else if(proxy.getHost()!=null && !proxy.getHost().isEmpty() && proxy.getPort()>0) {
                for(String prefix:new String[]{"HTTP","HTTPS"}) {
                    result.put(prefix+"Enable",1); result.put(prefix+"Proxy",proxy.getHost()); result.put(prefix+"Port",proxy.getPort());
                }
                result.put("ExceptionsList",new org.json.JSONArray(proxy.getExclusionList()));
            }
            return result.toString();
        } catch(Exception failure) {
            Log.w("I2AHost","System proxy settings unavailable: "+failure.getClass().getSimpleName());
            return null;
        }
    }
    private int networkFlags() {
        try{ConnectivityManager manager = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            Network network = manager==null ? null : manager.getActiveNetwork();
            NetworkCapabilities capabilities = network == null ? null : manager.getNetworkCapabilities(network);
            if (capabilities == null) return 0;
            return 2 | (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) ? 0x40000 : 0);
        }catch(RuntimeException unavailable){return 0;}
    }
    private boolean batteryMonitorRegistered;
    private int guestThermalStatus;
    private PowerManager.OnThermalStatusChangedListener thermalMonitor;
    private native void updateThermalSnapshot(int status);
    private void monitorPlatformChanges() {
        if(Build.VERSION.SDK_INT>=29) {
            PowerManager power=getSystemService(PowerManager.class);
            if(power!=null)try {
                guestThermalStatus=power.getCurrentThermalStatus();
                thermalMonitor=status->{guestThermalStatus=status;if(libraryLoaded && !isDestroyed())updateThermalSnapshot(status);};
                power.addThermalStatusListener(getMainExecutor(),thermalMonitor);
            } catch(RuntimeException unavailable) {thermalMonitor=null;}
        }
        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        try{if (Build.VERSION.SDK_INT >= 33) registerReceiver(batteryMonitor, filter, Context.RECEIVER_NOT_EXPORTED);
            else registerReceiver(batteryMonitor, filter);batteryMonitorRegistered=true;
        }catch(RuntimeException unavailable){Log.w("I2AHost","Battery observer unavailable",unavailable);}
        ConnectivityManager manager = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        if(manager==null)return;
        networkMonitor = new ConnectivityManager.NetworkCallback() {
            private void update() { mainHandler.post(() -> { if (guestLoaded && !isDestroyed()) networkChanged(networkFlags()); }); }
            @Override public void onAvailable(Network network) { update(); }
            @Override public void onLost(Network network) { update(); }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) { update(); }
        };
        try{manager.registerDefaultNetworkCallback(networkMonitor);}catch(RuntimeException unavailable){networkMonitor=null;Log.w("I2AHost","Network observer unavailable",unavailable);}
    }

    @Override public void onDestroy() {
        if(Build.VERSION.SDK_INT>=29 && thermalMonitor!=null){PowerManager power=getSystemService(PowerManager.class);if(power!=null)try{power.removeThermalStatusListener(thermalMonitor);}catch(RuntimeException ignored){}thermalMonitor=null;}
        if(batteryMonitorRegistered){try{unregisterReceiver(batteryMonitor);}catch(IllegalArgumentException ignored){}batteryMonitorRegistered=false;}
        if(networkMonitor!=null){ConnectivityManager manager=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);if(manager!=null)try{manager.unregisterNetworkCallback(networkMonitor);}catch(RuntimeException ignored){}networkMonitor=null;}
        if(motion!=null)motion.close();
        if(frameScheduler!=null)frameScheduler.close();
        for (GuestModal dialog:customGuestModals.values()) dialog.dismiss(false,() -> {});
        customGuestModals.clear();
        if(tableContent!=null)tableContent.close();
        if(webContent!=null)webContent.close();
        if(textEditors!=null)textEditors.close();
        NotificationScheduler.detach(notificationDelivery);
        completeNotificationPermissions();
        for (android.net.nsd.NsdManager.RegistrationListener listener : networkServices.values()) {
            try { ((android.net.nsd.NsdManager)getSystemService(Context.NSD_SERVICE)).unregisterService(listener); } catch (RuntimeException ignored) {}
        }
        networkServices.clear();
        synchronized(this){processActivitiesClosed=true;for(Long identifier:processActivities.keySet())processActivity(identifier,0,false);}
        closeGuestAccessibility();
        for(ModalPresenter.ControllerPanel panel:new java.util.ArrayList<>(controllerPanels.values()))panel.close();controllerPanels.clear();
        if(viewComposition!=null)viewComposition.close();pendingCompositions.clear();pendingLayers.clear();
        if(guestLaunch!=null) { guestLaunch.release(); guestLaunch=null; }
        if(audioSession!=null) audioSession.close();
        sound.close();
        if (photos != null) photos.close();
        if (locations != null) { locations.close(); closeLocationSources(); }
        if (shares != null) shares.close();
        if (helpshift != null) helpshift.close();
        super.onDestroy();
    }

    private boolean notificationsEnabled() {
        return NotificationScheduler.enabled(this);
    }

    private void cancelLocalNotifications() {
        NotificationScheduler.remove(this, null, false);
    }

    private static final int NOTIFICATION_PERMISSION = 7301;
    private final ArrayList<Long> notificationPermissions = new ArrayList<>();
    private String pendingNotificationResponse;
    private native String completeNotificationAuthorization(long block, boolean allowed);
    private native String deliverGuestNotification(String payload, boolean response);
    private final NotificationScheduler.Delivery notificationDelivery = (payload, response) -> {
        if (!guestKeyboardReady || isDestroyed() || (!response && activityState != 0)) return false;
        String failure = deliverGuestNotification(payload, response);
        if (failure != null) guestFailure(failure);
        return failure == null;
    };
    private final ConcurrentHashMap<Long, android.net.nsd.NsdManager.RegistrationListener> networkServices = new ConcurrentHashMap<>();
    private native String networkServiceEvent(long token, int event, int code, String name);
    private void publishNetworkService(long token, String type, String name, int port, byte[] txt, boolean noRename) {
        mainHandler.post(() -> {
            android.net.nsd.NsdManager manager = (android.net.nsd.NsdManager)getSystemService(Context.NSD_SERVICE);
            android.net.nsd.NsdServiceInfo info = new android.net.nsd.NsdServiceInfo();
            info.setServiceType(type); info.setServiceName(name); info.setPort(port);
            try {
                if (txt != null) for (int offset = 0; offset < txt.length;) {
                    int length = txt[offset++] & 255, end = offset + length, separator = offset;
                    if (end > txt.length || length == 0) throw new IllegalArgumentException();
                    while (separator < end && txt[separator] != '=') separator++;
                    String key = new String(txt, offset, separator-offset, StandardCharsets.US_ASCII);
                    String value = separator == end ? null : StandardCharsets.UTF_8.newDecoder()
                        .decode(ByteBuffer.wrap(txt, separator+1, end-separator-1)).toString();
                    info.setAttribute(key, value); offset = end;
                }
            } catch (RuntimeException | java.nio.charset.CharacterCodingException failure) { networkServiceCallback(token, 2, -72000, null); return; }
            android.net.nsd.NsdManager.RegistrationListener listener = new android.net.nsd.NsdManager.RegistrationListener() {
                private void event(int event, int code, String current) { mainHandler.post(() -> { if (networkServices.get(token) == this) networkServiceCallback(token, event, code, current); }); }
                @Override public void onRegistrationFailed(android.net.nsd.NsdServiceInfo service, int code) { event(2, code == android.net.nsd.NsdManager.FAILURE_ALREADY_ACTIVE ? -72003 : -72000, null); }
                @Override public void onServiceRegistered(android.net.nsd.NsdServiceInfo service) {
                    if (noRename && !name.equals(service.getServiceName())) { event(2, -72001, null); try { manager.unregisterService(this); } catch (RuntimeException ignored) {} }
                    else event(1, 0, service.getServiceName());
                }
                @Override public void onServiceUnregistered(android.net.nsd.NsdServiceInfo service) { event(3, 0, null); }
                @Override public void onUnregistrationFailed(android.net.nsd.NsdServiceInfo service, int code) { event(4, -72000, null); }
            };
            if (networkServices.putIfAbsent(token, listener) != null) { networkServiceCallback(token, 2, -72003, null); return; }
            try { manager.registerService(info, android.net.nsd.NsdManager.PROTOCOL_DNS_SD, listener); }
            catch (RuntimeException failure) { networkServices.remove(token, listener); networkServiceCallback(token, 2, -72000, null); }
        });
    }
    private void networkServiceCallback(long token, int event, int code, String name) {
        if (!guestLoaded || isDestroyed()) return;
        String failure = networkServiceEvent(token, event, code, name); if (failure != null) guestFailure(failure);
        if (event == 2 || event == 3) networkServices.remove(token);
    }
    private void stopNetworkService(long token) {
        mainHandler.post(() -> {
            android.net.nsd.NsdManager.RegistrationListener listener = networkServices.get(token);
            if (listener != null) {
                try { ((android.net.nsd.NsdManager)getSystemService(Context.NSD_SERVICE)).unregisterService(listener); }
                catch (RuntimeException failure) { networkServiceCallback(token, 4, -72000, null); }
            }
        });
    }
    private int notificationAuthorization() { return NotificationScheduler.authorization(this); }
    private void requestNotificationAuthorization(long block) {
        mainHandler.post(() -> {
            notificationPermissions.add(block);
            if (Build.VERSION.SDK_INT >= 33 && !notificationsEnabled() && !isDestroyed()) {
                if (notificationPermissions.size() == 1) {
                    NotificationScheduler.requested(this);
                    try{requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, NOTIFICATION_PERMISSION);}
                    catch(RuntimeException unavailable){completeNotificationPermissions();}
                }
            } else completeNotificationPermissions();
        });
    }
    private void completeNotificationPermissions() {
        ArrayList<Long> pending = new ArrayList<>(notificationPermissions);
        notificationPermissions.clear();
        for (long block : pending) {
            String failure = completeNotificationAuthorization(block, !isDestroyed() && notificationsEnabled());
            if (failure != null) guestFailure(failure);
        }
    }
    private String notificationOperation(String operation, String payload) {
        if (operation.equals("schedule")) return NotificationScheduler.schedule(this, payload);
        if (operation.equals("pending") || operation.equals("delivered")) return NotificationScheduler.query(this, operation.equals("delivered"));
        if (operation.equals("removePending") || operation.equals("removeDelivered")) NotificationScheduler.remove(this, payload, operation.equals("removeDelivered"));
        return null;
    }
    private void showLocalNotification(String payload, long options) { NotificationScheduler.show(this, payload, options); }
    private void receiveNotificationIntent(Intent intent) {
        String payload = NotificationScheduler.takeResponse(this, intent);
        if (payload != null) pendingNotificationResponse = payload;
        if (guestKeyboardReady && pendingNotificationResponse != null) {
            payload = pendingNotificationResponse;
            pendingNotificationResponse = null;
            notificationDelivery.receive(payload, true);
        }
    }

    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (frameScheduler != null) frameScheduler.refreshDisplay();
        if (guestLoaded) updateSystemSnapshot();
    }

    @Override public void onStart() {
        super.onStart();
        activityState = 1;
        updateGuestApplicationState();
    }

    @Override public void onResume() {
        super.onResume();
        if (helpshift != null) helpshift.foreground(true);
        activityState = 0;
        for (BackgroundTask task : backgroundTasks.values()) {
            mainHandler.removeCallbacks(task.expiration);
            if (task.wakeLock.isHeld()) task.wakeLock.release();
        }
        if (guestLoaded) updateSystemSnapshot();
        if (frameScheduler != null) frameScheduler.foreground(true);
    }

    @Override public void onPause() {
        if (helpshift != null) helpshift.foreground(false);
        cancelGuestTouches();
        activityState = 1;
        updateGuestApplicationState();
        if (frameScheduler != null) frameScheduler.foreground(false);
        super.onPause();
    }

    @Override public void onStop() {
        activityState = 2;
        for (BackgroundTask task : backgroundTasks.values()) armBackgroundTask(task);
        updateGuestApplicationState();
        if(libraryLoaded)operationWorkers.execute(this::persistPipelineCache);
        super.onStop();
    }

    private void scheduleOperation(long token, boolean mainQueue) {
        Runnable action = () -> {
            String failure = runOperation(token);
            if (failure != null) guestFailure(failure);
        };
        if (mainQueue) new Handler(Looper.getMainLooper()).post(action);
        else operationWorkers.execute(action);
    }

    private byte[] digest(String algorithm, byte[] input) throws NoSuchAlgorithmException {
        return MessageDigest.getInstance(algorithm).digest(input);
    }

    private String systemText(String key) {
        if (key.equals("model")) return Build.MODEL;
        if (key.equals("locale")) return Locale.getDefault().toString();
        if (key.equals("language")) return Locale.getDefault().getLanguage();
        if (key.equals("country")) return Locale.getDefault().getCountry();
        if (key.equals("androidVersion")) return Build.VERSION.RELEASE;
        if (key.equals("timezone")) return TimeZone.getDefault().getID();
        return null;
    }
    private String[] preferredLanguages() {
        android.os.LocaleList languages = getResources().getConfiguration().getLocales();
        String[] result = new String[languages.size()];
        for (int i = 0; i < result.length; i++) result[i] = languages.get(i).toLanguageTag();
        return result;
    }
    private String canonicalLocaleIdentifier(String identifier, boolean language) {
        android.icu.util.ULocale value = android.icu.util.ULocale.createCanonical(identifier);
        return language ? value.toLanguageTag() : value.getName();
    }
    private String localeIdentifierFromComponents(String[] parts) {
        android.icu.util.ULocale.Builder value = new android.icu.util.ULocale.Builder();
        if (parts[0] != null) value.setLanguage(parts[0]);
        if (parts[1] != null) value.setRegion(parts[1]);
        if (parts[2] != null) value.setScript(parts[2]);
        if (parts[3] != null) value.setVariant(parts[3]);
        return value.build().getName();
    }
    private String localeInfo(String identifier, String key) {
        android.icu.util.ULocale value = identifier == null ? android.icu.util.ULocale.getDefault() : android.icu.util.ULocale.createCanonical(identifier);
        key = key.replace("kCFLocale", "").replace("NSLocale", "");
        if (key.equalsIgnoreCase("localeIdentifier") || key.equalsIgnoreCase("Identifier")) return value.getName();
        if (key.equalsIgnoreCase("LanguageCode")) return value.getLanguage();
        if (key.equalsIgnoreCase("CountryCode")) return value.getCountry().isEmpty() ? null : value.getCountry();
        if (key.equalsIgnoreCase("ScriptCode")) return value.getScript().isEmpty() ? null : value.getScript();
        if (key.equalsIgnoreCase("VariantCode")) return value.getVariant().isEmpty() ? null : value.getVariant();
        android.icu.text.DecimalFormatSymbols symbols = android.icu.text.DecimalFormatSymbols.getInstance(value);
        if (key.equalsIgnoreCase("CurrencyCode")) return symbols.getInternationalCurrencySymbol();
        if (key.equalsIgnoreCase("CurrencySymbol")) return symbols.getCurrencySymbol();
        if (key.equalsIgnoreCase("DecimalSeparator")) return Character.toString(symbols.getDecimalSeparator());
        if (key.equalsIgnoreCase("GroupingSeparator")) return Character.toString(symbols.getGroupingSeparator());
        return null;
    }
    private int timezoneOffset(String name, long date) { return TimeZone.getTimeZone(name).getOffset(date) / 1000; }
    private static String comparisonText(String text, int options) {
        if ((options & 256) != 0) {
            StringBuilder normalized = new StringBuilder();
            for (int i = 0; i < text.length();) {
                char ch = text.charAt(i);
                if (ch == 0x3000 || ch >= 0xff00 && ch <= 0xffef) {
                    int end = i + 1;
                    while (end < text.length() && (text.charAt(end) == 0x3000 || text.charAt(end) >= 0xff00 && text.charAt(end) <= 0xffef)) ++end;
                    normalized.append(java.text.Normalizer.normalize(text.substring(i, end), java.text.Normalizer.Form.NFKC)); i = end;
                } else { normalized.append(ch); ++i; }
            }
            text = normalized.toString();
        }
        if ((options & 2) == 0 || (options & 128) != 0) text = java.text.Normalizer.normalize(text, java.text.Normalizer.Form.NFD);
        if ((options & 128) != 0) text = text.replaceAll("\\p{M}+", "");
        if ((options & 1) != 0) text = android.icu.lang.UCharacter.foldCase(text, true);
        if ((options & 4) != 0) text = new StringBuilder(text).reverse().toString();
        return text;
    }
    private static int naturalComparison(String first, String second) {
        int a = 0, b = 0;
        while (a < first.length() && b < second.length()) {
            char x = first.charAt(a), y = second.charAt(b);
            if (Character.isDigit(x) && Character.isDigit(y)) {
                int endA = a, endB = b;
                while (endA < first.length() && Character.isDigit(first.charAt(endA))) ++endA;
                while (endB < second.length() && Character.isDigit(second.charAt(endB))) ++endB;
                int startA = a, startB = b;
                while (startA < endA && Character.digit(first.charAt(startA), 10) == 0) ++startA;
                while (startB < endB && Character.digit(second.charAt(startB), 10) == 0) ++startB;
                if (endA - startA != endB - startB) return Integer.compare(endA - startA, endB - startB);
                while (startA < endA) {
                    int order = Character.digit(first.charAt(startA++), 10) - Character.digit(second.charAt(startB++), 10);
                    if (order != 0) return order;
                }
                a = endA; b = endB;
            } else { if (x != y) return x - y; ++a; ++b; }
        }
        return Integer.compare(first.length() - a, second.length() - b);
    }
    private int compareText(String first, String second, int options, String identifier) {
        String a = comparisonText(first, options), b = comparisonText(second, options);
        int order;
        if (identifier != null) {
            android.icu.text.RuleBasedCollator collator = (android.icu.text.RuleBasedCollator) android.icu.text.Collator.getInstance(android.icu.util.ULocale.createCanonical(identifier));
            collator.setNumericCollation((options & 64) != 0);
            collator.setDecomposition((options & 2) == 0 ? android.icu.text.Collator.CANONICAL_DECOMPOSITION : android.icu.text.Collator.NO_DECOMPOSITION);
            order = collator.compare(a, b);
        } else order = (options & 64) != 0 ? naturalComparison(a, b) : a.compareTo(b);
        return order == 0 && (options & 512) != 0 ? first.compareTo(second) : order;
    }
    private android.icu.text.NumberFormat numberFormatter(String identifier, int style, String positivePattern) {
        android.icu.util.ULocale locale = identifier == null ? android.icu.util.ULocale.getDefault() : android.icu.util.ULocale.createCanonical(identifier);
        android.icu.text.NumberFormat formatter = style == 2 ? android.icu.text.NumberFormat.getCurrencyInstance(locale) :
            style == 3 ? android.icu.text.NumberFormat.getPercentInstance(locale) : style == 4 ? android.icu.text.NumberFormat.getScientificInstance(locale) :
            android.icu.text.NumberFormat.getNumberInstance(locale);
        if (style == 0) formatter.setGroupingUsed(false);
        if (positivePattern != null && formatter instanceof android.icu.text.DecimalFormat) ((android.icu.text.DecimalFormat) formatter).applyPattern(positivePattern);
        return formatter;
    }
    private String localizedFormatNumber(long bits, char code, int flags, int width, int precision) {
        return LocalizedFormat.number(bits, code, flags, width, precision);
    }
    private String parseFormattedNumber(android.icu.text.NumberFormat formatter, String text) {
        text = text.trim(); java.text.ParsePosition position = new java.text.ParsePosition(0);
        Number number = formatter.parse(text, position);
        return number != null && position.getIndex() == text.length() ? number.toString() : null;
    }
    private LocationSource locations;
    private synchronized LocationSource locationSource(){
        if(locations==null)locations=new LocationSource(this,mainHandler,operationWorkers,this::deliverLocation,this::deliverLocationAuthorization,this::completeGeocode,this::guestFailure);
        return locations;
    }
    private native String deliverLocation(long token,int generation,double[] values,int code,String message);
    private native String deliverLocationAuthorization(long token,int status);
    private native String completeGeocode(long token,int generation,String places,int code,String message);
    private native void closeLocationSources();
    private int locationAuthorization(){return locationSource().authorization();}
    private boolean locationEnabled(){return locationSource().enabled();}
    private void requestLocationAuthorization(long token){locationSource().requestAuthorization(token);}
    private void startLocation(long token,int generation,double accuracy,double distance,boolean once){locationSource().start(token,generation,accuracy,distance,once);}
    private void stopLocation(long token){if(locations!=null)locations.stopAsync(token);}
    private double[] lastLocation(){return locationSource().lastLocation();}
    private double locationDistance(double firstLatitude,double firstLongitude,double latitude,double longitude){float[] distance=new float[1];android.location.Location.distanceBetween(firstLatitude,firstLongitude,latitude,longitude,distance);return distance[0];}
    private void geocode(long token,int generation,String address,double latitude,double longitude,double[] bounds,String language){locationSource().geocode(token,generation,address,latitude,longitude,bounds,language);}
    private void cancelGeocode(long token){if(locations!=null)locations.cancelGeocode(token);}
    private String[] telephonyInfo() {
        android.telephony.TelephonyManager phone = (android.telephony.TelephonyManager) getSystemService(TELEPHONY_SERVICE);
        if (phone == null || phone.getSimState() != android.telephony.TelephonyManager.SIM_STATE_READY) return null;
        String operator = phone.getSimOperator(), radio = null;
        String country = phone.getSimCountryIso(), name = phone.getSimOperatorName();
        String mcc = operator.length() >= 5 ? operator.substring(0, 3) : null;
        String mnc = operator.length() >= 5 ? operator.substring(3) : null;
        try {
            switch (phone.getDataNetworkType()) {
                case 1: radio = "CTRadioAccessTechnologyGPRS"; break;
                case 2: radio = "CTRadioAccessTechnologyEdge"; break;
                case 3: radio = "CTRadioAccessTechnologyWCDMA"; break;
                case 4: case 7: radio = "CTRadioAccessTechnologyCDMA1x"; break;
                case 5: radio = "CTRadioAccessTechnologyCDMAEVDORev0"; break;
                case 6: radio = "CTRadioAccessTechnologyCDMAEVDORevA"; break;
                case 8: case 10: case 15: radio = "CTRadioAccessTechnologyHSDPA"; break;
                case 9: radio = "CTRadioAccessTechnologyHSUPA"; break;
                case 12: radio = "CTRadioAccessTechnologyCDMAEVDORevB"; break;
                case 13: radio = "CTRadioAccessTechnologyLTE"; break;
                case 14: radio = "CTRadioAccessTechnologyeHRPD"; break;
                default: break;
            }
        } catch (SecurityException unavailable) {   }
        return new String[] {name.isEmpty() ? null : name, mcc, mnc, country.isEmpty() ? null : country, radio};
    }

    private byte[] vendorIdentifier() {
        String scoped = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
        if (scoped == null) {
            SharedPreferences runtime = getSharedPreferences(BuildProfile.RUNTIME_PREFERENCES, MODE_PRIVATE);
            scoped = runtime.getString("installation", null);
            if (scoped == null) { scoped = UUID.randomUUID().toString(); runtime.edit().putString("installation", scoped).commit(); }
        }
        UUID identifier = UUID.nameUUIDFromBytes((BuildProfile.UUID_NAMESPACE + scoped).getBytes(StandardCharsets.UTF_8));
        return ByteBuffer.allocate(16).putLong(identifier.getMostSignificantBits()).putLong(identifier.getLeastSignificantBits()).array();
    }

    private String resolveTimeZone(String name) {
        if (name.equals("PST") || name.equals("PDT")) name = "America/Los_Angeles";
        else if (name.equals("EST") || name.equals("EDT")) name = "America/New_York";
        else if (name.equals("MST") || name.equals("MDT")) name = "America/Denver";
        else if (name.equals("CST") || name.equals("CDT")) name = "America/Chicago";
        TimeZone zone = TimeZone.getTimeZone(name);
        if (zone.getID().equals("GMT") && !name.equals("GMT") && !name.equals("UTC") && !name.startsWith("GMT+") && !name.startsWith("GMT-")) return null;
        return zone.getID();
    }

    private byte[] characterBitmap(int kind) {
        byte[] bits = new byte[0x110000 / 8];
        for (int cp = 0; cp < 0x110000; cp++) {
            int type = Character.getType(cp);
            boolean mark = type == Character.NON_SPACING_MARK || type == Character.COMBINING_SPACING_MARK || type == Character.ENCLOSING_MARK;
            boolean letter = Character.isLetter(cp) || mark;
            boolean digit = type == Character.DECIMAL_DIGIT_NUMBER;
            boolean space = type == Character.SPACE_SEPARATOR || cp == 9;
            boolean newline = cp == 10 || cp == 11 || cp == 12 || cp == 13 || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
            boolean included = kind == 0 ? letter : kind == 1 ? mark : kind == 2 ? digit : kind == 3 ? letter || digit :
                kind == 4 ? space : kind == 5 ? space || newline : kind == 6 ? newline : type == Character.CONTROL || type == Character.FORMAT;
            if (included) bits[cp >>> 3] |= (byte) (1 << (cp & 7));
        }
        return bits;
    }

    private double dateFromComponents(int year, int month, int day, int hour, int minute, int second, String timezone) {
        GregorianCalendar calendar = new GregorianCalendar(TimeZone.getTimeZone(timezone), Locale.ROOT);
        calendar.clear();
        calendar.set(year, month - 1, day, hour, minute, second);
        return calendar.getTimeInMillis() / 1000.0;
    }

    private String transformText(String value, int mode) {
        if (mode == 0) return value.toLowerCase(Locale.ROOT);
        if (mode == 1) return value.toUpperCase(Locale.ROOT);
        String lower = value.toLowerCase(Locale.ROOT);
        BreakIterator words = BreakIterator.getWordInstance(Locale.ROOT);
        words.setText(lower);
        StringBuilder result = new StringBuilder(lower.length());
        int start = words.first();
        for (int end = words.next(); end != BreakIterator.DONE; start = end, end = words.next()) {
            int cp = lower.codePointAt(start);
            result.appendCodePoint(Character.toTitleCase(cp));
            result.append(lower, start + Character.charCount(cp), end);
        }
        return result.toString();
    }

    private long findText(String text, String part, int options, int start, int length) {
        int end = start + length;
        boolean backwards = (options & 4) != 0, anchored = (options & 8) != 0;
        long result = -1;
        if ((options & 1024) != 0) {
            int flags = (options & 1) == 0 ? 0 : Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE;
            Matcher matcher = Pattern.compile(part, flags).matcher(text).region(start, end);
            while (matcher.find()) {
                if (anchored && (backwards ? matcher.end() != end : matcher.start() != start)) continue;
                result = ((long) matcher.start() << 32) | (matcher.end() - matcher.start());
                if (!backwards) break;
            }
        } else if (part.length() <= length) {
            int first = start, last = end - part.length();
            for (int index = backwards ? last : first; index >= first && index <= last; index += backwards ? -1 : 1) {
                if (text.regionMatches((options & 1) != 0, index, part, 0, part.length()))
                    return ((long) index << 32) | part.length();
                if (anchored) break;
            }
        }
        return result;
    }

    private Pattern compileExpression(String pattern, int options) {
        int flags = 0;
        if ((options & 1) != 0) flags |= Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE;
        if ((options & 2) != 0) flags |= Pattern.COMMENTS;
        if ((options & 4) != 0) flags |= Pattern.LITERAL;
        if ((options & 8) != 0) flags |= Pattern.DOTALL;
        if ((options & 16) != 0) flags |= Pattern.MULTILINE;
        if ((options & 32) != 0) flags |= Pattern.UNIX_LINES;
        if ((options & 64) != 0) flags |= Pattern.UNICODE_CHARACTER_CLASS;
        return Pattern.compile(pattern, flags);
    }
    private long[] expressionMatches(Pattern pattern, String text, int options, int start, int length, int limit) {
        Matcher matcher = pattern.matcher(text).region(start, start + length)
            .useTransparentBounds((options & 8) != 0).useAnchoringBounds((options & 16) == 0);
        int groups = matcher.groupCount() + 1;
        ArrayList<long[]> matches = new ArrayList<>();
        while (matcher.find()) {
            if ((options & 4) != 0 && matcher.start() != start) break;
            long[] row = new long[groups * 2 + 1];
            row[0] = (matcher.hitEnd() ? 4 : 0) | (matcher.requireEnd() ? 8 : 0);
            for (int i = 0; i < groups; i++) { row[1 + i * 2] = matcher.start(i); row[2 + i * 2] = matcher.start(i) < 0 ? 0 : matcher.end(i) - matcher.start(i); }
            matches.add(row);
            if ((options & 4) != 0 || (limit > 0 && matches.size() >= limit)) break;
        }
        long[] result = new long[3 + matches.size() * (groups * 2 + 1)];
        result[0] = groups; result[1] = matches.size(); result[2] = (matcher.hitEnd() ? 4 : 0) | (matcher.requireEnd() ? 8 : 0);
        for (int i = 0; i < matches.size(); i++) System.arraycopy(matches.get(i), 0, result, 3 + i * (groups * 2 + 1), groups * 2 + 1);
        return result;
    }
    private String expressionReplace(Pattern pattern, String text, int options, int start, int length, String replacement) {
        Matcher matcher = pattern.matcher(text).region(start, start + length)
            .useTransparentBounds((options & 8) != 0).useAnchoringBounds((options & 16) == 0);
        StringBuffer result = new StringBuffer();
        while (matcher.find()) {
            if ((options & 4) != 0 && matcher.start() != start) break;
            matcher.appendReplacement(result, replacement);
            if ((options & 4) != 0) break;
        }
        matcher.appendTail(result);
        return result.toString();
    }

    private X509Certificate certificate(byte[] encoded) {
        try {
            return (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(new ByteArrayInputStream(encoded));
        } catch (java.security.cert.CertificateException invalid) {
            return null;
        }
    }

    private boolean validateTrust(X509Certificate[] chain, X509Certificate[] anchors, boolean anchorsOnly) {
        if (chain == null || chain.length == 0) return false;
        try {
            if (anchors != null && anchors.length != 0) {
                Set<TrustAnchor> roots = new HashSet<>();
                for (X509Certificate anchor : anchors) roots.add(new TrustAnchor(anchor, null));
                PKIXParameters parameters = new PKIXParameters(roots);
                parameters.setRevocationEnabled(false);
                X509Certificate[] path = chain;
                for (int index = 0; index < path.length; index++) {
                    boolean anchored = false;
                    for (X509Certificate anchor : anchors) if (chain[index].equals(anchor)) { anchored = true; break; }
                    if (anchored) { path = Arrays.copyOf(chain, index); break; }
                }
                try {
                    CertPathValidator.getInstance("PKIX").validate(
                        CertificateFactory.getInstance("X.509").generateCertPath(Arrays.asList(path)), parameters);
                    return true;
                } catch (java.security.GeneralSecurityException rejected) {
                    if (anchorsOnly) return false;
                }
            } else if (anchorsOnly) return false;
            TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            factory.init((KeyStore) null);
            for (TrustManager manager : factory.getTrustManagers()) {
                if (manager instanceof X509TrustManager) {
                    ((X509TrustManager) manager).checkServerTrusted(chain, chain[0].getPublicKey().getAlgorithm());
                    return true;
                }
            }
        } catch (java.security.GeneralSecurityException rejected) {
            return false;
        }
        return false;
    }

    private String installedApkPath(){return getApplicationInfo().sourceDir;}
    private synchronized String bundleFile(String relative) throws IOException {
        File root = new File(getCacheDir(), "bundle-files/" + BuildProfile.PACKAGE_REVISION + "/" + BuildProfile.BUNDLE_NAME);
        File target = new File(root, relative).getCanonicalFile();
        if (!target.getPath().startsWith(root.getCanonicalPath() + File.separator))
            throw new IOException("Invalid bundle path");
        if (!target.isFile()) {
            try (InputStream input = getAssets().open(BuildProfile.BUNDLE_NAME + "/" + relative)) {
                File parent = target.getParentFile();
                if (!parent.mkdirs() && !parent.isDirectory()) throw new IOException("Cannot create " + parent);
                File temp = File.createTempFile("i2a-", ".tmp", parent);
                try {
                    Files.copy(input, temp.toPath(), StandardCopyOption.REPLACE_EXISTING);
                    Files.move(temp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                    Log.i("I2AGuest","Bundle file materialized: "+relative+" ("+target.length()+" bytes)");
                } finally {
                    temp.delete();
                }
            } catch (java.io.FileNotFoundException missing) {
                return null;
            }
        }
        return target.getPath();
    }
    private static void removeCacheTree(File directory) throws IOException {
        long[] removed={0};
        Files.walkFileTree(directory.toPath(),new java.nio.file.SimpleFileVisitor<java.nio.file.Path>() {
            @Override public java.nio.file.FileVisitResult visitFile(java.nio.file.Path file,java.nio.file.attribute.BasicFileAttributes attributes) throws IOException {if(Files.deleteIfExists(file))removed[0]+=attributes.size();return java.nio.file.FileVisitResult.CONTINUE;}
            @Override public java.nio.file.FileVisitResult postVisitDirectory(java.nio.file.Path path,IOException failure) throws IOException {if(failure!=null)throw failure;Files.deleteIfExists(path);return java.nio.file.FileVisitResult.CONTINUE;}
        });
        Log.i("I2AGuest","Obsolete runtime cache removed: "+directory.getName()+" ("+removed[0]+" bytes)");
    }
    private static void pruneCacheVersions(File root,String current) {
        File[] children=root.listFiles();if(children==null)return;
        for(File child:children)if(!child.getName().equals(current))try{removeCacheTree(child);}catch(IOException failure){Log.w("I2AGuest","Unable to remove obsolete runtime cache: "+child.getName(),failure);}
    }
    private void cleanRuntimeCaches() {
        operationWorkers.execute(()-> {
            File[] legacy=getCacheDir().listFiles();if(legacy!=null)for(File child:legacy)if(child.getName().matches("bundle-(?:[0-9a-f]{12}|.+-[0-9a-f]{20})"))try{removeCacheTree(child);}catch(IOException failure){Log.w("I2AGuest","Unable to remove obsolete bundle cache",failure);}
            pruneCacheVersions(new File(getCacheDir(),"bundle-files"),BuildProfile.PACKAGE_REVISION);
            pruneCacheVersions(new File(getCodeCacheDir(),"vulkan"),BuildProfile.PACKAGE_REVISION);
        });
    }
    private synchronized String shaderCacheDirectory(long expectedSize) {
        try {
            File root=new File(getCodeCacheDir(),"vulkan/"+BuildProfile.PACKAGE_REVISION);
            if(!root.mkdirs() && !root.isDirectory())throw new IOException("Unable to create the shader cache");
            File packed=new File(root,"shaders.bin");
            if(!packed.isFile() || packed.length()!=expectedSize){
                File temporary=File.createTempFile("shaders-",".tmp",root);
                try(InputStream input=getAssets().open("i2a/shaders.bin")) {
                    Files.copy(input,temporary.toPath(),StandardCopyOption.REPLACE_EXISTING);
                    if(temporary.length()!=expectedSize)throw new IOException("Shader pack size differs from the runtime metadata");
                    Files.move(temporary.toPath(),packed.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                } finally {temporary.delete();}
            }
            return root.getAbsolutePath();
        } catch(IOException failure){Log.w("I2AGuest","Shader disk cache unavailable; using APK resources",failure);return null;}
    }
    private native void persistPipelineCache();
    private void schedulePipelineCacheSave(){mainHandler.postDelayed(()->operationWorkers.execute(this::persistPipelineCache),1000);}

    private boolean bundleExists(String relative) {
        if (relative.startsWith("/") || Arrays.asList(relative.split("/")).contains("..")) return false;
        String path = BuildProfile.BUNDLE_NAME + "/" + relative;
        try {
            if (getAssets().list(path).length != 0) return true;
            try (InputStream ignored = getAssets().open(path)) { return true; }
        } catch (IOException missing) {
            return false;
        }
    }

    private final ConcurrentHashMap<String,String[]> bundleResourceLanguages=new ConcurrentHashMap<>();
    private static int resourceLanguageScore(String supported,Locale wanted) {
        android.icu.util.ULocale expected=android.icu.util.ULocale.addLikelySubtags(android.icu.util.ULocale.forLocale(wanted));
        android.icu.util.ULocale actual=android.icu.util.ULocale.addLikelySubtags(android.icu.util.ULocale.forLanguageTag(supported.replace('_','-')));
        if(supported.equalsIgnoreCase(wanted.getDisplayLanguage(Locale.ENGLISH)))return 1;
        if(!expected.getLanguage().equals(actual.getLanguage()) || !expected.getScript().equals(actual.getScript()))return -1;
        return supported.equalsIgnoreCase(wanted.toLanguageTag()) ? 100 : expected.getCountry().equals(actual.getCountry()) ? 10 : 1;
    }
    private String localizedBundleResource(String root,String filename,String development) throws IOException {
        boolean assets=root.equals(BuildProfile.BUNDLE_NAME) || root.startsWith(BuildProfile.BUNDLE_NAME+"/");
        if(!assets && !new File(root).isAbsolute())return null;
        if(filename.startsWith("/") || filename.contains(".lproj/") || Arrays.asList(filename.split("/")).contains("..") || Arrays.asList(root.split("/")).contains(".."))return null;
        android.os.LocaleList preferred=getResources().getConfiguration().getLocales();
        String key=root+"|"+preferred.toLanguageTags()+"|"+development;
        String[] ordered=bundleResourceLanguages.get(key);
        if(ordered==null){
            ArrayList<String> available=new ArrayList<>(),selected=new ArrayList<>();
            String[] folders=assets ? getAssets().list(root) : new File(root).list();
            if(folders!=null)for(String folder:folders)if(folder.endsWith(".lproj") && !folder.equals("Base.lproj"))available.add(folder);
            java.util.Collections.sort(available);
            for(int i=0;i<preferred.size();i++){
                Locale wanted=preferred.get(i);ArrayList<String> matches=new ArrayList<>();
                for(String folder:available)if(resourceLanguageScore(folder.substring(0,folder.length()-6),wanted)>=0)matches.add(folder);
                matches.sort((a,b)->Integer.compare(resourceLanguageScore(b.substring(0,b.length()-6),wanted),resourceLanguageScore(a.substring(0,a.length()-6),wanted)));
                for(String folder:matches)if(!selected.contains(folder))selected.add(folder);
            }
            if(development!=null && !development.isEmpty()){
                String folder=development+".lproj";if(!selected.contains(folder))selected.add(folder);
            }
            if(!selected.contains("Base.lproj"))selected.add("Base.lproj");
            ordered=selected.toArray(new String[0]);String[] prior=bundleResourceLanguages.putIfAbsent(key,ordered);if(prior!=null)ordered=prior;
        }
        for(String folder:ordered){String path=root+"/"+folder+"/"+filename;if(!assets){if(new File(path).isFile())return path;continue;}try(InputStream ignored=getAssets().open(path)){return path;}catch(java.io.FileNotFoundException missing){}}
        return null;
    }

    private int bundleEntryType(String relative) {
        if (relative.startsWith("/") || Arrays.asList(relative.split("/")).contains("..")) return 0;
        try {
            String path = relative.isEmpty() ? BuildProfile.BUNDLE_NAME : BuildProfile.BUNDLE_NAME + "/" + relative;
            if (getAssets().list(path).length != 0) return 2;
            try (InputStream ignored = getAssets().open(path)) { return 1; }
        } catch (IOException missing) { return 0; }
    }

    private String[] bundleList(String relative) {
        if (relative.startsWith("/") || Arrays.asList(relative.split("/")).contains("..")) return null;
        try { return getAssets().list(relative.isEmpty() ? BuildProfile.BUNDLE_NAME : BuildProfile.BUNDLE_NAME + "/" + relative); }
        catch (IOException missing) { return null; }
    }

    private int preference(String key, int operation) {
        SharedPreferences store = getSharedPreferences(BuildProfile.DEFAULTS_DOMAIN, MODE_PRIVATE);
        if (operation == -2 && !store.contains(key)) return -1;
        if (operation < 0) return store.getBoolean(key, false) ? 1 : 0;
        SharedPreferences.Editor editor = store.edit();
        if (operation == 2) editor.remove(key);
        else editor.putBoolean(key, operation != 0);
        return editor.commit() ? 1 : 0;
    }

    private String legacyDefaults(String domain) {
        return new JSONObject(getSharedPreferences(domain, MODE_PRIVATE).getAll()).toString();
    }
    private String compatibilityDirectory() {
        File directory = getExternalFilesDir("compatibility");
        return (directory != null ? directory : getFilesDir()).getAbsolutePath();
    }

    @Override public void onCreate(Bundle state) {
        CrashLog.initialize(this);
        super.onCreate(state);
        cleanRuntimeCaches();
        NotificationScheduler.attach(notificationDelivery);
        receiveNotificationIntent(getIntent());
        photos = new PhotoLibrary(this, mainHandler, operationWorkers, (token, code, description) -> {
            String failure = completePhotoWrite(token, code, description);
            if (failure != null) guestFailure(failure);
        },new PhotoLibrary.Events(){
            public void selected(long token,int result,String metadata,byte[] bytes){String failure=completePhotoPicker(token,result,metadata,bytes);if(failure!=null)guestFailure(failure);}
            public void read(long token,byte[] bytes,String metadata,int error){String failure=completePhotoRead(token,bytes,metadata,error);if(failure!=null)guestFailure(failure);}
            public void authorized(long token,int result){String failure=completePhotoAuthorization(token,result);if(failure!=null)guestFailure(failure);}
        });
        motion = new MotionSource(this, this::deliverDeviceMotion, this::guestFailure);
        keychain = new CredentialStore(this);
        audioSession = new AudioSession(this, mainHandler, sound, () -> guestLoaded, this::guestFailure);
        monitorPlatformChanges();
        frameScheduler = new FrameScheduler(this, this::guestFailure);
        TextView status = new TextView(this);
        guestStatus = status;
        status.setGravity(Gravity.CENTER);
        status.setVisibility(android.view.View.GONE);
        android.widget.FrameLayout content = new android.widget.FrameLayout(this);
        guestContent = content;
        guestKeyboard = new KeyboardState(content, () -> guestKeyboardReady && !startupFailed,
                this::updateGuestKeyboard, this::guestFailure);
        android.view.SurfaceView surface = new android.view.SurfaceView(this);
        surface.setKeepScreenOn(true);
        surface.setOnTouchListener((view, event) -> guestTouch(event));
        content.addView(surface, new android.widget.FrameLayout.LayoutParams(-1, -1));

        guestLabelOverlay = new LabelOverlay(this);
        guestLabelOverlay.setClipChildren(false);
        guestLabelOverlay.setClipToPadding(false);
        content.addView(guestLabelOverlay, new android.widget.FrameLayout.LayoutParams(-1, -1));
        try { guestLaunch=new LaunchScreenView(this); content.addView(guestLaunch,new android.widget.FrameLayout.LayoutParams(-1,-1)); }
        catch(org.json.JSONException | RuntimeException failure) { guestFailure("Unable to restore the original launch screen: "+failure.getMessage()); }
        content.addView(status, new android.widget.FrameLayout.LayoutParams(-1, -1));
        surface.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            @Override public void surfaceCreated(android.view.SurfaceHolder holder) { frameScheduler.surface(holder.getSurface()); }
            @Override public void surfaceChanged(android.view.SurfaceHolder holder, int format, int width, int height) {
                frameScheduler.surface(holder.getSurface());
                if (!libraryLoaded) return;
                setRenderSurface(holder.getSurface(), width, height);
                if (!guestStarted) {
                    guestStarted = true;
                    status.post(() -> {
                        guestLoaded = true;
                        updateSystemSnapshot();
                        String error = startGuest(getFilesDir().getAbsolutePath());
                        if (error != null) guestFailure(error);
                        else { guestKeyboardReady = true; guestKeyboard.refresh(); receiveNotificationIntent(getIntent()); }
                    });
                }
            }
            @Override public void surfaceDestroyed(android.view.SurfaceHolder holder) {
                frameScheduler.surface(null);
                cancelGuestTouches();
                if (libraryLoaded) setRenderSurface(null, 0, 0);
            }
        });
        setContentView(content);
         
        getWindow().getDecorView().setOnApplyWindowInsetsListener((view, insets) -> {
            updateGuestStatusBarSnapshot(insets);
            return view.onApplyWindowInsets(insets);
        });
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            content.post(() -> {
                android.view.WindowInsetsController insets = getWindow().getDecorView().getWindowInsetsController();
                if (insets != null) {
                    insets.hide(android.view.WindowInsets.Type.systemBars());
                    insets.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            });
        } else {
            getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                android.view.View.SYSTEM_UI_FLAG_FULLSCREEN | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
        try {
            System.loadLibrary("i2a");
            CrashLog.attach(HostActivity::configureCrashLog);
            libraryLoaded = true;
            openGuestAccessibility();
            // DEVELOPMENT_BEGIN
            configureBusinessDiagnostics(getIntent());
            // DEVELOPMENT_END
        } catch (LinkageError error) {
            CrashLog.record("Android linker", error);
            Log.e("I2AGuest", "Android linker", error);
            guestFailure(error.toString());
        }
    }
}
