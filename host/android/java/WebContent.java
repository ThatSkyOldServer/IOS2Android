package local.ios.host;

import android.app.Activity;
import android.os.Build;
import android.view.View;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceError;
import android.webkit.WebResourceResponse;
import android.webkit.JsResult;
import android.webkit.JsPromptResult;
import android.widget.FrameLayout;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.lang.reflect.Method;
import java.nio.charset.Charset;
import java.util.HashMap;
import java.util.Collections;
import java.util.Locale;
import java.io.IOException;
import java.net.URLConnection;

final class WebContent {
    interface Callback {
        long event(long view, int kind, long operation, String data);
        void alpha(long view, View target, float value);
        void detached(long view);
    }
    private final Activity activity;
    private final LabelOverlay overlay;
    private final Callback callback;
    private final HashMap<Long, Page> pages = new HashMap<>();
    private long dialogSequence;
    private boolean closed;
    private static Method addStartScript, removeStartScript;
    private static boolean scriptSupport, scriptSupportKnown;

    WebContent(Activity activity, LabelOverlay overlay, Callback callback) {
        this.activity=activity; this.overlay=overlay; this.callback=callback;
    }
    private static void scriptSupport() {
        if (scriptSupportKnown) return;
        scriptSupportKnown=true;
        try {
            Class<?> feature=Class.forName("androidx.webkit.WebViewFeature");
            scriptSupport=(Boolean)feature.getMethod("isFeatureSupported",String.class).invoke(null,"DOCUMENT_START_SCRIPT");
            if (scriptSupport) {
                addStartScript=Class.forName("androidx.webkit.WebViewCompat").getMethod("addDocumentStartJavaScript",WebView.class,String.class,java.util.Set.class);
                removeStartScript=Class.forName("androidx.webkit.ScriptHandler").getMethod("remove");
            }
        } catch (ReflectiveOperationException | LinkageError failure) { scriptSupport=false; }
    }
    private long event(Page page,int kind,long operation,JSONObject data) { return callback.event(page.token,kind,operation,data.toString()); }
    private static JSONObject fields(Object... fields) {
        JSONObject result=new JSONObject();
        try { for(int i=0;i<fields.length;i+=2) result.put((String)fields[i],fields[i+1]); }
        catch(org.json.JSONException failure) { throw new IllegalArgumentException(failure); }
        return result;
    }
    private Page page(long token) {
        Page page=pages.get(token);
        if(page==null) { page=new Page(token); pages.put(token,page); }
        return page;
    }
    void sync(long token,LabelOverlay parent,float[] metrics,int[] options) {
            if(closed || activity.isDestroyed())return;
            Page page=page(token);
            if(options[1]==0) {
                LabelOverlay.detach(page.panel); callback.detached(token);
                return;
            }
            LabelOverlay.attach(parent,page.panel);
            int width=Math.max(0,(int)Math.ceil(metrics[2])),height=Math.max(0,(int)Math.ceil(metrics[3]));
            android.view.ViewGroup.LayoutParams previous=page.panel.getLayoutParams();
            if(previous==null || previous.width!=width || previous.height!=height)page.panel.setLayoutParams(new FrameLayout.LayoutParams(width,height));
            page.panel.setTranslationX(metrics[0]);page.panel.setTranslationY(metrics[1]);
            parent.depth(page.panel,metrics[5],metrics[6]);
            page.panel.setClipBounds(new android.graphics.Rect((int)metrics[7],(int)metrics[8],(int)Math.ceil(metrics[7]+metrics[9]),(int)Math.ceil(metrics[8]+metrics[10])));
            page.panel.setVisibility(options[0]!=0 ? View.VISIBLE : View.GONE);page.enabled=options[2]!=0;page.background=options[3];
            if(page.web!=null){page.web.setEnabled(page.enabled);page.web.setBackgroundColor(page.background);}
            parent.visibility(page.panel,options[0]!=0 && metrics[4]>0);
            callback.alpha(token,page.panel,metrics[4]);
    }
    View view(long token) { Page page=pages.get(token);return page==null ? null : page.panel; }
    void command(long token,String action,String input) {
            if(closed || activity.isDestroyed())return;
            if(action.equals("close")) { Page removed=pages.remove(token);if(removed!=null)removed.destroy();return; }
            Page page=page(token);
            try {
                JSONObject data=input==null ? new JSONObject() : new JSONObject(input);
                if(action.equals("configure")) { page.configure(data);return; }
                if(action.equals("scroll")) {
                    page.scrolling=data.optBoolean("enabled",true);page.vertical=data.optBoolean("vertical",true);page.horizontal=data.optBoolean("horizontal",true);
                    float density=activity.getResources().getDisplayMetrics().density;
                    page.left=(float)data.optDouble("left");page.top=(float)data.optDouble("top");
                    page.right=(float)data.optDouble("right");page.bottom=(float)data.optDouble("bottom");
                    if(page.web!=null){page.applyScroll();page.web.scrollTo(Math.round(((float)data.optDouble("x")+page.left)*density),Math.round(((float)data.optDouble("y")+page.top)*density));}return;
                }
                if(action.equals("viewport")){page.scales=data.optBoolean("scales");if(page.web!=null)page.applyViewport();return;}
                if(action.equals("dialog")) { page.resolveDialog(data.optLong("id"),data.optBoolean("accepted"),data.optString("value"));return; }
                if(action.equals("evaluate")) {
                    if(page.web==null || page.configurationFailed){event(page,9,data.getLong("operation"),fields("code",page.web==null?2:4,"error",page.web==null?"The page process exited.":"The web view configuration is unavailable."));return;}
                    long operation=data.getLong("operation");String source=JSONObject.quote(data.getString("source"));
                    String script="(function(){try{var v=(0,eval)("+source+");if(typeof v==='function'||typeof v==='symbol'||(v&&typeof v.then==='function'))return {code:5,error:'Unsupported JavaScript result'};JSON.stringify(v);return {value:v===undefined?null:v};}catch(e){return {code:4,error:String(e)};}})()";
                    page.web.evaluateJavascript(script,value->callback.event(token,9,operation,value==null ? "null" : value));return;
                }
                if(action.equals("stop")) { if(page.web!=null)page.web.stopLoading();page.loading=false;page.started=false;page.snapshot();return; }
                if(action.equals("back") || action.equals("forward")) {
                    if(page.web!=null){if(action.equals("back") && page.web.canGoBack())page.web.goBack();else if(action.equals("forward") && page.web.canGoForward())page.web.goForward();}return;
                }
                if(page.web==null){page.createWeb();page.configure(page.settings);}
                if(page.configurationFailed){page.fail(-1,"The web view configuration is unavailable.",page.url);return;}
                if(!page.configured)page.configure(new JSONObject());
                page.started=false;page.loading=true;page.failed=false;page.error.setVisibility(View.GONE);page.web.setVisibility(View.VISIBLE);page.web.stopLoading();
                if(action.equals("html") || action.equals("data")) {
                    String mime=data.optString("mime","text/html"),encoding=data.optString("encoding","UTF-8"),base=data.optString("url","about:blank");
                    byte[] bytes=android.util.Base64.decode(data.getString("bytes"),android.util.Base64.DEFAULT);
                    if(!mime.startsWith("text/") && !mime.contains("javascript") && !mime.contains("json") && !mime.contains("xml")) { page.fail(-1000,"This content type is unavailable.",base);return; }
                    page.web.loadDataWithBaseURL(base,new String(bytes,Charset.forName(encoding)),mime,"UTF-8",base);return;
                }
                String url=data.getString("url"),method=data.optString("method","GET");page.url=url;
                if(!url.startsWith("https://") && !url.startsWith("http://") && !url.equals("about:blank")) { page.fail(-1002,"Unsupported page URL.",url);return; }
                if(method.equals("POST")) {
                    JSONObject headers=data.optJSONObject("headers");
                    if(headers!=null && headers.length()!=0) { page.fail(-1000,"Custom headers on POST navigation are unavailable.",url);return; }
                    page.web.postUrl(url,android.util.Base64.decode(data.optString("bytes",""),android.util.Base64.DEFAULT));
                } else if(method.equals("GET")) {
                    HashMap<String,String> headers=new HashMap<>();JSONObject values=data.optJSONObject("headers");
                    if(values!=null)for(java.util.Iterator<String> keys=values.keys();keys.hasNext();) { String key=keys.next();headers.put(key,values.getString(key)); }
                    page.web.loadUrl(url,headers);
                } else page.fail(-1000,"This navigation method is unavailable.",url);
            } catch(Exception | LinkageError failure) { page.fail(-1,"Unable to load this page.",page.url); }
    }
    void close() { closed=true;for(Page page:pages.values())page.destroy();pages.clear(); }

    private final class Page {
        final long token;
        final FrameLayout panel;
        final TextView error;
        DocumentView web;
        final HashMap<Long,JsResult> dialogs=new HashMap<>();
        final HashMap<Long,ModalPresenter> panels=new HashMap<>();
        Object scriptHandler;
        JSONArray endScripts;
        JSONObject settings=new JSONObject();
        boolean configured,configurationFailed,started,loading,failed,destroyed,scales,enabled=true,scrolling=true,vertical=true,horizontal=true;
        int background=0xffffffff;
        String url="about:blank";
        float left,top,right,bottom;
        private final class DocumentView extends WebView {
            DocumentView(){super(activity);}
            @Override public boolean onTouchEvent(android.view.MotionEvent event){if(!scrolling && event.getActionMasked()==android.view.MotionEvent.ACTION_MOVE)return true;return super.onTouchEvent(event);}
            int contentWidth(){return computeHorizontalScrollRange();}
            int contentHeight(){return computeVerticalScrollRange();}
        }
        Page(long token) {
            this.token=token;panel=new FrameLayout(activity);
            error=new TextView(activity);error.setText("Unable to load this page. Tap to retry.");error.setGravity(android.view.Gravity.CENTER);error.setTextColor(new ModalPresenter.Palette(activity).foreground);
            error.setBackgroundColor(new ModalPresenter.Palette(activity).surface);error.setVisibility(View.GONE);error.setOnClickListener(view->event(this,0,0,fields("url",url,"type",3)));
            panel.addView(error,new FrameLayout.LayoutParams(-1,-1));
        }
        void createWeb() {
            try{
            web=new DocumentView();
            web.getSettings().setAllowFileAccess(false);web.getSettings().setAllowContentAccess(false);web.getSettings().setDomStorageEnabled(true);
            if(Build.VERSION.SDK_INT>=33)try{web.getSettings().setAlgorithmicDarkeningAllowed(new ModalPresenter.Palette(activity).dark);}catch(UnsupportedOperationException | LinkageError unavailable){}
            web.setEnabled(enabled);web.setBackgroundColor(background);applyScroll();applyViewport();
            panel.addView(web,0,new FrameLayout.LayoutParams(-1,-1));
            web.setWebViewClient(new WebViewClient() {
                @Override public WebResourceResponse shouldInterceptRequest(WebView view,WebResourceRequest request) { return bundleResource(request); }
                @Override public boolean shouldOverrideUrlLoading(WebView view,WebResourceRequest request) {
                    if(!request.isForMainFrame())return false;
                    event(Page.this,0,0,fields("url",request.getUrl().toString(),"type",request.hasGesture()?0:5));return true;
                }
                @Override public void onPageStarted(WebView view,String url,android.graphics.Bitmap icon) {
                    if(destroyed || web!=view)return;Page.this.url=url;
                    boolean redirect=started;started=loading=true;failed=false;event(Page.this,redirect?2:1,0,fields("url",url));snapshot();
                }
                @Override public void onPageCommitVisible(WebView view,String url) { if(!destroyed && web==view && started && !failed)event(Page.this,3,0,fields("url",url)); }
                @Override public void onPageFinished(WebView view,String url) {
                    if(destroyed || web!=view || !started || failed)return;loading=false;
                    if(!scriptSupport && endScripts!=null)for(int i=0;i<endScripts.length();i++)web.evaluateJavascript(script(endScripts.optJSONObject(i)),null);
                    event(Page.this,4,0,fields("url",url));snapshot();
                }
                @Override public void onReceivedError(WebView view,WebResourceRequest request,WebResourceError failure) {
                    if(!destroyed && web==view && request.isForMainFrame()) { int code=failure.getErrorCode();fail(code==ERROR_TIMEOUT?-1001:code==ERROR_HOST_LOOKUP?-1003:code==ERROR_CONNECT?-1004:-1,failure.getDescription().toString(),request.getUrl().toString()); }
                }
                @Override public void onReceivedHttpError(WebView view,WebResourceRequest request,WebResourceResponse response) { if(request.isForMainFrame())event(Page.this,14,0,fields("status",response.getStatusCode(),"url",request.getUrl().toString())); }
                @Override public boolean onRenderProcessGone(WebView view,android.webkit.RenderProcessGoneDetail detail) {
                    if(destroyed || web!=view)return true;
                    if(loading)fail(-1020,"The page process exited.",url);else error.setVisibility(View.VISIBLE);
                    event(Page.this,15,0,new JSONObject());cancelDialogs();removeScripts();panel.removeView(view);web=null;configured=false;view.destroy();return true;
                }
            });
            web.setWebChromeClient(new WebChromeClient() {
                @Override public void onProgressChanged(WebView view,int progress) { if(!destroyed && web==view)event(Page.this,7,0,fields("progress",progress/100.0)); }
                @Override public void onReceivedTitle(WebView view,String title) { if(!destroyed && web==view)event(Page.this,6,0,fields("title",title)); }
                @Override public boolean onJsAlert(WebView view,String url,String message,JsResult result) { dialog(11,message,"",result);return true; }
                @Override public boolean onJsConfirm(WebView view,String url,String message,JsResult result) { dialog(12,message,"",result);return true; }
                @Override public boolean onJsPrompt(WebView view,String url,String message,String value,JsPromptResult result) { dialog(13,message,value,result);return true; }
                @Override public void onCloseWindow(WebView view) { event(Page.this,10,0,new JSONObject()); }
            });
            web.setOnScrollChangeListener((view,x,y,oldX,oldY)->snapshot());
            }catch(RuntimeException | LinkageError unavailable){if(web!=null){DocumentView failed=web;web=null;panel.removeView(failed);try{failed.destroy();}catch(RuntimeException ignored){}}throw unavailable;}
        }
        void applyScroll(){float density=activity.getResources().getDisplayMetrics().density;web.setVerticalScrollBarEnabled(vertical);web.setHorizontalScrollBarEnabled(horizontal);web.setPadding(Math.round(left*density),Math.round(top*density),Math.round(right*density),Math.round(bottom*density));}
        void applyViewport(){web.getSettings().setUseWideViewPort(scales);web.getSettings().setLoadWithOverviewMode(scales);}
        void snapshot() {
            if(web==null)return;
            float density=activity.getResources().getDisplayMetrics().density;
            event(this,8,0,fields("x",web.getScrollX()/density-left,"y",web.getScrollY()/density-top,"width",web.contentWidth()/density,"height",web.contentHeight()/density,"back",web.canGoBack(),"forward",web.canGoForward(),"loading",loading));
        }
        String script(JSONObject entry) { if(entry==null)return "";String source=entry.optString("source");return (entry.optBoolean("main",true)?"if(window.top===window){":"{")+source+"\n}"; }
        void configure(JSONObject data) throws Exception {
            settings=data;configured=false;configurationFailed=true;
            if(web==null)createWeb();
            web.getSettings().setJavaScriptEnabled(data.optBoolean("javascript",true));web.getSettings().setMinimumFontSize(Math.max(0,data.optInt("minimumFont")));
            web.getSettings().setMediaPlaybackRequiresUserGesture(data.optBoolean("mediaGesture",true));
            String agent=data.optString("userAgent");String suffix=data.optString("agentSuffix");
            web.getSettings().setUserAgentString(agent.isEmpty()?android.webkit.WebSettings.getDefaultUserAgent(activity)+(suffix.isEmpty()?"":" "+suffix):agent);
            scriptSupport();removeScripts();
            JSONArray scripts=data.optJSONArray("scripts");endScripts=new JSONArray();StringBuilder start=new StringBuilder(),end=new StringBuilder();
            if(scripts!=null)for(int i=0;i<scripts.length();i++) { JSONObject entry=scripts.getJSONObject(i);if(entry.optInt("time")==0)start.append(script(entry)).append('\n');else { endScripts.put(entry);end.append(script(entry)).append('\n'); } }
            if(start.length()!=0 && !scriptSupport)throw new IllegalStateException("Document-start JavaScript is unavailable.");
            if(scriptSupport && (start.length()!=0 || end.length()!=0)) {
                if(end.length()!=0)start.append("document.addEventListener('DOMContentLoaded',function(){").append(end).append("\n},{once:true});");
                scriptHandler=addStartScript.invoke(null,web,start.toString(),Collections.singleton("*"));
            }
            configured=true;configurationFailed=false;
        }
        void fail(int code,String description,String url) { loading=false;failed=true;started=false;event(this,5,0,fields("code",code,"description",description,"url",url));error.setVisibility(View.VISIBLE);snapshot(); }
        void dialog(int kind,String message,String value,JsResult result) {
            if(destroyed || web==null){result.cancel();return;}
            long identifier=++dialogSequence;dialogs.put(identifier,result);
            if(event(this,kind,identifier,fields("message",message,"value",value))!=0)return;
            String[] answer={value};String[] actions=kind==11?new String[]{"OK"}:new String[]{"OK","Cancel"};
            ModalPresenter prompt=new ModalPresenter(activity,1,actions.length-1,"",message,actions,new boolean[actions.length],new int[actions.length],kind==13?answer:new String[0],kind==13?new String[]{""}:new String[0],kind==13?new int[]{0}:new int[0],(index,text)-> { answer[0]=text;return text; },index->resolveDialog(identifier,index==0,answer[0]));
            for(int i=0;i<actions.length;i++)prompt.actionEnabled(i,true);panels.put(identifier,prompt);prompt.show();
        }
        void resolveDialog(long identifier,boolean accepted,String value) { JsResult result=dialogs.remove(identifier);ModalPresenter prompt=panels.remove(identifier);if(prompt!=null)prompt.dismiss();if(result==null)return;if(!accepted)result.cancel();else if(result instanceof JsPromptResult)((JsPromptResult)result).confirm(value);else result.confirm(); }
        void cancelDialogs(){for(JsResult result:dialogs.values())result.cancel();dialogs.clear();ModalPresenter[] active=panels.values().toArray(new ModalPresenter[0]);panels.clear();for(ModalPresenter prompt:active)prompt.dismiss();}
        void removeScripts(){if(scriptHandler!=null){try{removeStartScript.invoke(scriptHandler);}catch(Exception ignored){}scriptHandler=null;}}
        void destroy() {
            destroyed=true;cancelDialogs();removeScripts();
            callback.detached(token);LabelOverlay.detach(panel);if(web!=null){panel.removeView(web);web.stopLoading();web.destroy();web=null;}
        }
    }
    private WebResourceResponse bundleResource(WebResourceRequest request) {
        android.net.Uri uri=request.getUrl();
        if(!"https".equals(uri.getScheme()) || !"appassets.androidplatform.net".equals(uri.getHost()))return null;
        String path=uri.getPath();
        if(!request.getMethod().equals("GET") || path==null || !path.startsWith("/bundle/"))return new WebResourceResponse("text/plain","UTF-8",404,"Not Found",Collections.emptyMap(),null);
        String relative=path.substring(8);
        for(String component:relative.split("/"))if(component.equals("..") || component.equals(".") || component.indexOf('\\')>=0 || component.indexOf('\0')>=0)return new WebResourceResponse("text/plain","UTF-8",404,"Not Found",Collections.emptyMap(),null);
        String mime=URLConnection.guessContentTypeFromName(relative);
        if(relative.toLowerCase(Locale.ROOT).endsWith(".js") || relative.toLowerCase(Locale.ROOT).endsWith(".mjs"))mime="application/javascript";
        if(mime==null)mime="application/octet-stream";
        try{return new WebResourceResponse(mime,mime.startsWith("text/") || mime.equals("application/javascript")?"UTF-8":null,activity.getAssets().open(BuildProfile.BUNDLE_NAME+"/"+relative));}
        catch(IOException missing){return new WebResourceResponse("text/plain","UTF-8",404,"Not Found",Collections.emptyMap(),null);}
    }
}
