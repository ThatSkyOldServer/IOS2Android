package local.ios.host;

import android.app.Activity;
import android.app.Dialog;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Typeface;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.net.Uri;
import android.util.Log;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.function.IntConsumer;

final class ModalPresenter extends Dialog {
    interface Edit { String replace(int field, String value); }
    private final Activity activity;
    private final ArrayList<Button> buttons = new ArrayList<>();
    private final int surface, foreground, muted, accent, fieldColor;
    private final boolean sheet;
    private final int cancelIndex;
    private final IntConsumer select;
    private final ScrollView contentScroll;
    private android.window.OnBackInvokedCallback back;
    private int dp(float value) { return Math.round(value * activity.getResources().getDisplayMetrics().density); }
    private static int theme(Activity activity,String name) {
        return activity.getResources().getIdentifier(name,"style",activity.getPackageName());
    }
    static final class Palette {
        final boolean dark;
        final int surface, foreground, muted, accent, field;
        Palette(Activity activity) {
            dark=(activity.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)==Configuration.UI_MODE_NIGHT_YES;
            surface=color(activity,dark ? "system_neutral1_900" : "system_neutral1_50",dark ? 0xff24242a : 0xfff8f5fc);
            foreground=color(activity,dark ? "system_neutral1_50" : "system_neutral1_900",dark ? 0xfff1edf5 : 0xff211e28);
            muted=color(activity,dark ? "system_neutral2_200" : "system_neutral2_700",dark ? 0xffc7c2cf : 0xff57515e);
            accent=color(activity,dark ? "system_accent1_200" : "system_accent1_600",dark ? 0xffd0bcff : 0xff6750a4);
            field=color(activity,dark ? "system_neutral2_800" : "system_neutral2_100",dark ? 0xff343039 : 0xffe8e2ec);
        }
        private static int color(Activity activity,String name,int fallback) {
            if (Build.VERSION.SDK_INT < 31) return fallback;
            int id=activity.getResources().getIdentifier(name,"color","android");
            return id==0 ? fallback : activity.getColor(id);
        }
    }
    private GradientDrawable shape(int color, int radius) {
        GradientDrawable result=new GradientDrawable(); result.setColor(color); result.setCornerRadius(dp(radius)); return result;
    }
    private TextView text(String value, float size, int color) {
        TextView view=new TextView(activity); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setFontFeatureSettings("kern"); view.setIncludeFontPadding(false); return view;
    }
    private LinearLayout.LayoutParams row(int top) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2); p.topMargin=dp(top); return p;
    }
    ModalPresenter(Activity activity, int style, int cancel, String title, String message, String[] actions, boolean[] enabled, int[] styles,
               String[] values, String[] hints, int[] options, Edit edit, IntConsumer select) {
        super(activity, theme(activity,"HostDialogTheme"));
        this.activity=activity;
        sheet=style==0; cancelIndex=cancel; this.select=select;
        Palette palette=new Palette(activity); boolean dark=palette.dark;
        surface=palette.surface; foreground=palette.foreground; muted=palette.muted; accent=palette.accent; fieldColor=palette.field;
        setCancelable(cancel>=0); setCanceledOnTouchOutside(sheet && cancel>=0);
        ScrollView scroll=new ScrollView(activity); scroll.setFillViewport(false); scroll.setClipToPadding(false);
        contentScroll=scroll;
        LinearLayout panel=new LinearLayout(activity); panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(24),dp(24),dp(24),dp(24)); panel.setBackground(shape(surface,28));
        panel.setFocusableInTouchMode(true);
        if (title!=null && !title.isEmpty()) {
            TextView heading=text(title,24,foreground); heading.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
            heading.setAccessibilityHeading(true); panel.addView(heading,row(0));
        }
        if (message!=null && !message.isEmpty()) {
            TextView body=text(message,16,muted); body.setLineSpacing(dp(4),1); panel.addView(body,row(panel.getChildCount()==0 ? 0 : 16));
        }
        for (int index=0; index<values.length; ++index) {
            final int field=index;
            if (hints[index]!=null && !hints[index].isEmpty()) panel.addView(text(hints[index],12,muted),row(20));
            EditText input=new EditText(activity); input.setSingleLine(true); input.setTextSize(16); input.setTextColor(foreground); input.setHintTextColor(muted);
            TextInput ime=new TextInput(input);
            input.setMinHeight(dp(56)); input.setPadding(dp(16),dp(12),dp(16),dp(12));
            GradientDrawable background=shape(fieldColor,12); input.setBackground(background);
            input.setOnFocusChangeListener((v,focused)->{ background.setStroke(dp(focused ? 2 : 1),focused ? accent : fieldColor); });
            int type=TextInput.inputType(options[index*5],options[index*5+2],options[index*5+3]!=0);
            input.setInputType(type); input.setEnabled(options[index*5+4]!=0);
            int key=options[index*5+1];
            input.setImeOptions(TextInput.options(key));
            input.setOnClickListener(v -> ime.requestShow());
            input.setText(values[index]); input.setHint(hints[index]); input.setSelectAllOnFocus(true);
            input.addTextChangedListener(new TextWatcher() {
                boolean syncing;
                public void beforeTextChanged(CharSequence s,int start,int count,int after) {}
                public void onTextChanged(CharSequence s,int start,int before,int count) {}
                public void afterTextChanged(Editable value) {
                    if (syncing) return;
                    String accepted=edit.replace(field,value.toString());
                    if (accepted!=null && !accepted.contentEquals(value)) {
                        syncing=true; input.setText(accepted); input.setSelection(accepted.length()); syncing=false;
                    }
                }
            });
            panel.addView(input,row(8));
        }
        boolean inline=!sheet && actions.length<=2;
        LinearLayout actionList=new LinearLayout(activity); actionList.setOrientation(inline ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL); actionList.setGravity(Gravity.END);
        for (int index=0;index<actions.length;++index) {
            final int action=index; Button button=new Button(activity); button.setText(actions[index]); button.setAllCaps(false);
            button.setTextSize(14); button.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
            button.setMinHeight(dp(48)); button.setMinimumHeight(dp(48)); button.setPadding(dp(20),dp(8),dp(20),dp(8));
            boolean primary=actions.length==1;
            int color=styles[index]==2 ? (dark ? 0xffffb4ab : 0xffba1a1a) : accent;
            int ink=primary ? surface : color;
            button.setTextColor(new ColorStateList(new int[][]{new int[]{android.R.attr.state_enabled},new int[]{}},new int[]{ink,muted}));
            GradientDrawable fill=shape(primary ? color : fieldColor,24);
            button.setBackground(new RippleDrawable(ColorStateList.valueOf((foreground&0xffffff)|0x24000000),fill,null));
            button.setStateListAnimator(null); button.setElevation(0); button.setEnabled(enabled[index]);
            button.setOnClickListener(v->{ if (isShowing()) select.accept(action); });
            LinearLayout.LayoutParams layout=inline ? new LinearLayout.LayoutParams(-2,-2) : row(index==0 ? 0 : 8);
            if(inline && index>0) layout.setMarginStart(dp(8));
            buttons.add(button); actionList.addView(button,layout);
        }
        if (actions.length!=0) panel.addView(actionList,row(24));
        scroll.addView(panel); setContentView(scroll);
        Window window=getWindow(); window.setBackgroundDrawableResource(android.R.color.transparent);
        window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND); window.setDimAmount(0.45f);
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        window.setGravity(sheet ? Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL : Gravity.CENTER);
        if (Build.VERSION.SDK_INT>=30) {
            scroll.setOnApplyWindowInsetsListener((view,insets)-> {
                android.graphics.Insets safe=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout()|WindowInsets.Type.ime());
                layoutWindow(safe.left,safe.top,safe.right,safe.bottom); return insets;
            });
        } else scroll.setFitsSystemWindows(true);
        panel.requestFocus();
    }
    @Override public void show() {
        super.show();
        Window window=getWindow();
        if (Build.VERSION.SDK_INT>=30) {
            android.graphics.Insets safe=activity.getWindowManager().getCurrentWindowMetrics().getWindowInsets()
                .getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout()|WindowInsets.Type.ime());
            layoutWindow(safe.left,safe.top,safe.right,safe.bottom);
            WindowInsetsController controller=window.getInsetsController();
            if (controller!=null) {
                controller.show(WindowInsets.Type.systemBars());
                int mask=WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS|WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                boolean dark=(activity.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)==Configuration.UI_MODE_NIGHT_YES;
                controller.setSystemBarsAppearance(dark ? 0 : mask,mask);
            }
            window.getDecorView().requestApplyInsets();
        } else layoutWindow(0,0,0,0);
        if (Build.VERSION.SDK_INT>=33 && cancelIndex>=0 && back==null) {
            back=this::cancel; window.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,back);
        }
    }
    private void layoutWindow(int left,int top,int right,int bottom) {
        Window window=getWindow();
        android.graphics.Rect bounds=Build.VERSION.SDK_INT>=30 ? activity.getWindowManager().getCurrentWindowMetrics().getBounds() :
            new android.graphics.Rect(0,0,activity.getResources().getDisplayMetrics().widthPixels,activity.getResources().getDisplayMetrics().heightPixels);
        int width=Math.min(dp(sheet ? 640 : 560),Math.max(dp(48),bounds.width()-left-right-dp(sheet ? 32 : 48)));
        int maxHeight=Math.max(dp(48),bounds.height()-top-bottom-dp(48));
        contentScroll.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(maxHeight,View.MeasureSpec.AT_MOST));
        WindowManager.LayoutParams attributes=window.getAttributes();
        int height=contentScroll.getMeasuredHeight(),offset=sheet ? bottom+dp(16) : 0;
        if(attributes.width!=width || attributes.height!=height || attributes.y!=offset) {
            attributes.width=width; attributes.height=height; attributes.y=offset; window.setAttributes(attributes);
        }
    }
    @Override public void cancel() { if (cancelIndex>=0 && cancelIndex<buttons.size() && buttons.get(cancelIndex).isEnabled() && isShowing()) select.accept(-2); }
    @Override public void onBackPressed() { cancel(); }
    @Override public void dismiss() {
        if (Build.VERSION.SDK_INT>=33 && back!=null) { getWindow().getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(back); back=null; }
        super.dismiss();
    }
    void actionEnabled(int index, boolean enabled) { if (index>=0 && index<buttons.size()) buttons.get(index).setEnabled(enabled); }

    interface ControllerCallback { void event(int event,int width,int height);boolean canDismiss(); }
    static final class ControllerPanel extends Dialog {
        private final Activity activity;private final Palette palette;private final ControllerCallback callback;private final LabelOverlay root;private final int style;private final float[] anchor;private float preferredWidth,preferredHeight;private boolean closed,closing,notified;private android.window.OnBackInvokedCallback back;
        private int dp(float value){return Math.round(value*activity.getResources().getDisplayMetrics().density);}
        ControllerPanel(Activity activity,int style,float width,float height,float[] anchor,int arrows,ControllerCallback callback){
            super(activity,theme(activity,"HostPanelTheme"));this.activity=activity;this.style=style;this.preferredWidth=width;this.preferredHeight=height;this.anchor=anchor;this.callback=callback;palette=new Palette(activity);
            root=new LabelOverlay(activity){@Override protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);if(!closed && w>0 && h>0 && (w!=oldw || h!=oldh))callback.event(0,w,h);}};root.setFocusableInTouchMode(true);
            if(style==1 || style==2 || style==7){GradientDrawable shape=new GradientDrawable();shape.setColor(palette.surface);shape.setCornerRadius(dp(style==7 ? 16 : 28));root.setBackground(shape);root.setClipToOutline(true);}else root.setBackgroundColor(palette.surface);
            setContentView(root);setCancelable(true);setCanceledOnTouchOutside(style==1 || style==2 || style==7);Window window=getWindow();window.setBackgroundDrawableResource(android.R.color.transparent);window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);window.setDimAmount(style==7 ? .25f : .4f);if(style==1 || style==2 || style==7)window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            if(Build.VERSION.SDK_INT>=30){window.setDecorFitsSystemWindows(false);root.setOnApplyWindowInsetsListener((view,insets)->{android.graphics.Insets safe=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout()|WindowInsets.Type.ime());layout(safe.left,safe.top,safe.right,safe.bottom);return insets;});}else root.setFitsSystemWindows(true);
        }
        LabelOverlay overlay(){return root;}
        private void layout(int left,int top,int right,int bottom){if(closed)return;Window window=getWindow();Rect bounds=Build.VERSION.SDK_INT>=30 ? activity.getWindowManager().getCurrentWindowMetrics().getBounds() : new Rect(0,0,activity.getResources().getDisplayMetrics().widthPixels,activity.getResources().getDisplayMetrics().heightPixels);int margin=style==1 || style==2 || style==7 ? dp(16) : 0;
            int availableWidth=Math.max(1,bounds.width()-left-right-margin*2),availableHeight=Math.max(1,bounds.height()-top-bottom-margin*2);int width=availableWidth,height=availableHeight;if(style==1 || style==2 || style==7){width=Math.min(availableWidth,preferredWidth>0 ? Math.round(preferredWidth) : dp(style==7 ? 320 : 640));height=Math.min(availableHeight,preferredHeight>0 ? Math.round(preferredHeight) : style==7 ? dp(320) : Math.round(availableHeight*.9f));}
            int x=left+(bounds.width()-left-right-width)/2,y=top+(bounds.height()-top-bottom-height)/2;if(style==1)y=bounds.height()-bottom-margin-height;
            if(style==7 && anchor!=null){x=Math.max(left+margin,Math.min(bounds.width()-right-margin-width,Math.round(anchor[0]+anchor[2]/2-width/2)));int below=Math.round(anchor[1]+anchor[3]+dp(8)),above=Math.round(anchor[1]-dp(8)-height);y=below+height<=bounds.height()-bottom-margin ? below : Math.max(top+margin,above);}
            WindowManager.LayoutParams attributes=window.getAttributes();if(attributes.width!=width || attributes.height!=height || attributes.x!=x || attributes.y!=y || attributes.gravity!=(Gravity.TOP|Gravity.LEFT)){attributes.width=width;attributes.height=height;attributes.gravity=Gravity.TOP|Gravity.LEFT;attributes.x=x;attributes.y=y;window.setAttributes(attributes);}
        }
        void resize(float width,float height,float dim){preferredWidth=width;preferredHeight=height;Window window=getWindow();if(dim>0){window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);window.setDimAmount(dim);}else window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);applyInsets();}
        private void applyInsets(){if(Build.VERSION.SDK_INT>=30){android.graphics.Insets safe=activity.getWindowManager().getCurrentWindowMetrics().getWindowInsets().getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout()|WindowInsets.Type.ime());layout(safe.left,safe.top,safe.right,safe.bottom);root.requestApplyInsets();}else layout(0,0,0,0);}
        void show(boolean animated){super.show();applyInsets();Window window=getWindow();if(Build.VERSION.SDK_INT>=30){WindowInsetsController controller=window.getInsetsController();if(controller!=null){int mask=WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS|WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;controller.setSystemBarsAppearance(palette.dark ? 0 : mask,mask);}}if(Build.VERSION.SDK_INT>=33 && back==null){back=this::cancel;window.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,back);}
            root.setAlpha(animated ? 0 : 1);root.post(()->{if(closed || closing)return;callback.event(0,root.getWidth(),root.getHeight());if(animated)root.animate().alpha(1).setDuration(200).withEndAction(this::shown).start();else shown();});}
        private void shown(){if(closed || closing || notified)return;notified=true;callback.event(1,root.getWidth(),root.getHeight());}
        @Override public void cancel(){if(!closed && !closing && callback.canDismiss())callback.event(2,0,0);}
        @Override public void onBackPressed(){cancel();}
        void dismiss(boolean animated){if(closed || closing)return;closing=true;root.animate().cancel();if(animated)root.animate().alpha(0).setDuration(150).withEndAction(()->finish(3)).start();else finish(3);}
        void close(){if(!closed)finish(4);}
        private void finish(int event){if(closed)return;closed=true;root.animate().cancel();if(Build.VERSION.SDK_INT>=33 && back!=null){getWindow().getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(back);back=null;}super.dismiss();callback.event(event,0,0);root.removeAllViews();}
    }
    static final class WebPanel extends Dialog {
        private WebView web;
        private final LinearLayout content;
        private final Palette palette;
        private final ProgressBar progress;
        private final TextView failure;
        private android.window.OnBackInvokedCallback back;
        private boolean failed;
        private int dp(float value) { return Math.round(value*getContext().getResources().getDisplayMetrics().density); }
        WebPanel(Activity activity,String title) {
            super(activity,theme(activity,"HostPanelTheme")); palette=new Palette(activity);
            content=new LinearLayout(getContext()); content.setOrientation(LinearLayout.VERTICAL); content.setBackgroundColor(palette.surface);
            LinearLayout toolbar=new LinearLayout(getContext()); toolbar.setGravity(Gravity.CENTER_VERTICAL); toolbar.setPadding(dp(8),dp(4),dp(16),dp(4));
            ImageButton close=new ImageButton(getContext()); close.setContentDescription("Back"); close.setTooltipText("Back");
            close.setImageResource(activity.getResources().getIdentifier("back","drawable",activity.getPackageName()));
            close.setImageTintList(ColorStateList.valueOf(palette.foreground)); close.setPadding(dp(12),dp(12),dp(12),dp(12));
            GradientDrawable mask=new GradientDrawable(); mask.setColor(palette.surface); mask.setCornerRadius(dp(24));
            close.setBackground(new RippleDrawable(ColorStateList.valueOf((palette.foreground&0xffffff)|0x24000000),null,mask));
            close.setOnClickListener(view->navigateBack()); toolbar.addView(close,new LinearLayout.LayoutParams(dp(48),dp(48)));
            TextView heading=new TextView(getContext()); heading.setText(title); heading.setTextColor(palette.foreground); heading.setTextSize(22);
            heading.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL)); heading.setAccessibilityHeading(true);
            LinearLayout.LayoutParams headingLayout=new LinearLayout.LayoutParams(-1,-2); headingLayout.setMarginStart(dp(16)); toolbar.addView(heading,headingLayout);
            content.addView(toolbar,new LinearLayout.LayoutParams(-1,dp(64)));
            progress=new ProgressBar(getContext(),null,android.R.attr.progressBarStyleHorizontal); progress.setProgressTintList(ColorStateList.valueOf(palette.accent));
            progress.setProgressBackgroundTintList(ColorStateList.valueOf(palette.field)); content.addView(progress,new LinearLayout.LayoutParams(-1,dp(3)));
            failure=new TextView(getContext()); failure.setTextColor(palette.foreground); failure.setTextSize(16); failure.setGravity(Gravity.CENTER);
            failure.setPadding(dp(24),dp(24),dp(24),dp(24)); failure.setVisibility(View.GONE); failure.setFocusable(true);
            failure.setOnClickListener(view->{ if(web!=null) web.reload(); }); content.addView(failure,new LinearLayout.LayoutParams(-1,0,1));
            try{
            web=new WebView(getContext()); web.setBackgroundColor(palette.surface);
            web.getSettings().setJavaScriptEnabled(true); web.getSettings().setDomStorageEnabled(true);
            web.getSettings().setAllowFileAccess(false); web.getSettings().setAllowContentAccess(false);
            if(Build.VERSION.SDK_INT>=33)try{web.getSettings().setAlgorithmicDarkeningAllowed(palette.dark);}catch(UnsupportedOperationException | LinkageError unavailable){}
            web.setWebChromeClient(new WebChromeClient() {
                @Override public void onProgressChanged(WebView view,int value) {
                    progress.setProgress(value); progress.setVisibility(value==100 || failed ? View.GONE : View.VISIBLE);
                }
            });
            web.setWebViewClient(new WebViewClient() {
                @Override public boolean shouldOverrideUrlLoading(WebView view,WebResourceRequest request) {
                    Uri uri=request.getUrl();
                    if("https".equalsIgnoreCase(uri.getScheme())) return false;
                    if("mailto".equalsIgnoreCase(uri.getScheme()) || "tel".equalsIgnoreCase(uri.getScheme())) {
                        try { activity.startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW,uri)); } catch(RuntimeException ignored) {}
                    }
                    return true;
                }
                @Override public void onPageStarted(WebView view,String url,android.graphics.Bitmap icon) {
                    failed=false; failure.setVisibility(View.GONE); view.setVisibility(View.VISIBLE); progress.setVisibility(View.VISIBLE);
                }
                @Override public void onPageFinished(WebView view,String url) {
                    if(!failed) Log.i("I2AHost","Web panel page loaded");
                }
                @Override public void onReceivedError(WebView view,WebResourceRequest request,WebResourceError error) {
                    if(request.isForMainFrame()) unavailable("Unable to load this page. Check your connection and tap to retry.",error.getErrorCode());
                }
                @Override public void onReceivedHttpError(WebView view,WebResourceRequest request,WebResourceResponse response) {
                    if(request.isForMainFrame()) unavailable("Service unavailable. Tap to retry.",response.getStatusCode());
                }
                @Override public boolean onRenderProcessGone(WebView view,android.webkit.RenderProcessGoneDetail detail){
                    if(web==view){web=null;content.removeView(view);view.destroy();unavailable("The web page stopped. Close this window and reopen it to retry.",-1);}return true;
                }
            });
            content.addView(web,new LinearLayout.LayoutParams(-1,0,1));
            }catch(RuntimeException | LinkageError unavailable){if(web!=null){WebView broken=web;web=null;content.removeView(broken);try{broken.destroy();}catch(RuntimeException ignored){}}unavailable("Web content is unavailable on this device.",-1);}
            setContentView(content);
            Window window=getWindow(); window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            if(Build.VERSION.SDK_INT>=30) {
                window.setDecorFitsSystemWindows(false);
                content.setOnApplyWindowInsetsListener((view,insets)-> {
                    android.graphics.Insets bars=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout());
                    android.graphics.Insets keyboard=insets.getInsets(WindowInsets.Type.ime());
                    view.setPadding(bars.left,bars.top,bars.right,Math.max(bars.bottom,keyboard.bottom)); return insets;
                });
            } else content.setFitsSystemWindows(true);
        }
        private void unavailable(String message,int code) {
            failed=true; progress.setVisibility(View.GONE); if(web!=null)web.setVisibility(View.GONE);
            failure.setText(message); failure.setVisibility(View.VISIBLE); Log.w("I2AHost","Web panel load failed: "+code);
        }
        void load(Uri uri) { if(web!=null)try{web.loadUrl(uri.toString());}catch(RuntimeException | LinkageError unavailable){unavailable("Unable to load this page.",-1);} }
        private void navigateBack() { if(web!=null && web.canGoBack()) web.goBack(); else dismiss(); }
        @Override public void onBackPressed() { navigateBack(); }
        @Override public void show() {
            super.show(); Window window=getWindow(); window.setLayout(-1,-1);
            if(Build.VERSION.SDK_INT>=30) {
                WindowInsetsController controller=window.getInsetsController();
                if(controller!=null) {
                    controller.show(WindowInsets.Type.systemBars());
                    int mask=WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS|WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                    controller.setSystemBarsAppearance(palette.dark ? 0 : mask,mask);
                }
                content.requestApplyInsets();
            }
            if(Build.VERSION.SDK_INT>=33 && back==null) {
                back=this::navigateBack; window.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,back);
            }
        }
        @Override public void dismiss() {
            if(Build.VERSION.SDK_INT>=33 && back!=null) { getWindow().getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(back); back=null; }
            if(web!=null) { web.stopLoading(); content.removeView(web); web.destroy(); web=null; }
            super.dismiss();
        }
    }
}
