package local.ios.host;

import android.os.Build;
import android.text.InputType;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

final class TextInput implements View.OnAttachStateChangeListener,
        ViewTreeObserver.OnGlobalLayoutListener, ViewTreeObserver.OnWindowFocusChangeListener {
    private final EditText editor;
    private final Runnable show = this::showWhenReady;
    private ViewTreeObserver observer;
    private boolean pending;
    // DEVELOPMENT_BEGIN
    private int diagnosticSequence;
    private static final class ShowResult extends android.os.ResultReceiver {
        private final java.lang.ref.WeakReference<TextInput> owner;
        private final int sequence;
        ShowResult(TextInput ime, int sequence) {
            super(ime.editor.getHandler()); this.owner = new java.lang.ref.WeakReference<>(ime); this.sequence = sequence;
        }
        @Override protected void onReceiveResult(int code, android.os.Bundle data) {
            TextInput ime = owner.get();
            if (ime != null && sequence == ime.diagnosticSequence)
                android.util.Log.i("I2AGuest", "[DIAG-ime] request=" + sequence + " result=" + code +
                        " (0=alreadyShown,1=stillHidden,2=shown,3=hidden)");
        }
    }
    // DEVELOPMENT_END

    TextInput(EditText editor) {
        this.editor = editor;
        editor.setFocusableInTouchMode(true);
        editor.setShowSoftInputOnFocus(true);
        editor.addOnAttachStateChangeListener(this);
    }

    static int inputType(int keyboard, int capitals, boolean secure) {
        int type = InputType.TYPE_CLASS_TEXT;
        if (keyboard == 3) type |= InputType.TYPE_TEXT_VARIATION_URI;
        else if (keyboard == 7) type |= InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
        else if (keyboard == 4 || keyboard == 11) type = InputType.TYPE_CLASS_NUMBER;
        else if (keyboard == 5) type = InputType.TYPE_CLASS_PHONE;
        else if (keyboard == 8) type = InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL;
        else if (keyboard == 6) type |= InputType.TYPE_TEXT_VARIATION_PERSON_NAME;
        else if (keyboard == 10) type |= InputType.TYPE_TEXT_VARIATION_WEB_EDIT_TEXT;
        if (secure) {
            if ((type & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_NUMBER)
                return type | InputType.TYPE_NUMBER_VARIATION_PASSWORD;
            return InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
        }
         
        if ((type & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT) {
            if (capitals == 1) type |= InputType.TYPE_TEXT_FLAG_CAP_WORDS;
            else if (capitals == 2) type |= InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
            else if (capitals == 3) type |= InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS;
        }
        return type;
    }

    static int options(int returnKey) {
        int action = returnKey == 1 ? EditorInfo.IME_ACTION_GO :
                returnKey == 4 ? EditorInfo.IME_ACTION_NEXT :
                returnKey == 6 ? EditorInfo.IME_ACTION_SEARCH :
                returnKey == 7 ? EditorInfo.IME_ACTION_SEND : EditorInfo.IME_ACTION_DONE;
        return action | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
    }

    void requestShow() {
        pending = true;
        // DEVELOPMENT_BEGIN
        ++diagnosticSequence;
        android.util.Log.i("I2AGuest", "[DIAG-ime] request=" + diagnosticSequence +
                " attached=" + editor.isAttachedToWindow() + " windowFocus=" + editor.hasWindowFocus());
        // DEVELOPMENT_END
        listen();
        editor.requestFocus();
        schedule();
    }

    void cancel() {
        pending = false;
        editor.removeCallbacks(show);
        unlisten();
        // DEVELOPMENT_BEGIN
        ++diagnosticSequence;
        // DEVELOPMENT_END
    }

    private void listen() {
        if (!pending || !editor.isAttachedToWindow()) return;
        ViewTreeObserver current = editor.getViewTreeObserver();
        if (observer == current) return;
        unlisten(); observer = current;
        observer.addOnGlobalLayoutListener(this);
        observer.addOnWindowFocusChangeListener(this);
    }

    private void unlisten() {
        if (observer != null && observer.isAlive()) {
            observer.removeOnGlobalLayoutListener(this);
            observer.removeOnWindowFocusChangeListener(this);
        }
        observer = null;
    }

    private void schedule() {
        editor.removeCallbacks(show);
        if (pending && editor.isAttachedToWindow()) editor.post(show);
    }

    private void showWhenReady() {
        if (!pending || !editor.isAttachedToWindow() || !editor.isLaidOut() ||
                editor.isLayoutRequested() || !editor.hasWindowFocus() || !editor.isShown() ||
                !editor.isEnabled()) return;
        if (!editor.hasFocus() && !editor.requestFocus()) return;
        pending = false; unlisten();
        InputMethodManager manager = (InputMethodManager) editor.getContext().getSystemService(android.content.Context.INPUT_METHOD_SERVICE);
        android.os.ResultReceiver result = null;
        // DEVELOPMENT_BEGIN
        final int sequence = diagnosticSequence;
        result = new ShowResult(this, sequence);
        // DEVELOPMENT_END

        boolean accepted = manager.showSoftInput(editor, 0, result);
        // DEVELOPMENT_BEGIN
        android.util.Log.i("I2AGuest", "[DIAG-ime] request=" + sequence + " accepted=" + accepted +
                " served=" + manager.isActive(editor) + " focus=" + editor.hasFocus() +
                " windowFocus=" + editor.hasWindowFocus() + " size=" + editor.getWidth() + "x" + editor.getHeight() +
                " options=0x" + Integer.toHexString(editor.getImeOptions()));
        if (Build.VERSION.SDK_INT >= 30) editor.postDelayed(() -> {
            if (sequence != diagnosticSequence || !editor.isAttachedToWindow()) return;
            android.view.WindowInsets insets = editor.getRootWindowInsets();
            android.util.Log.i("I2AGuest", "[DIAG-ime] request=" + sequence +
                    " visible=" + (insets != null && insets.isVisible(android.view.WindowInsets.Type.ime())));
        }, 500);
        // DEVELOPMENT_END
    }

    @Override public void onViewAttachedToWindow(View view) { listen(); schedule(); }
    @Override public void onViewDetachedFromWindow(View view) { cancel(); }
    @Override public void onGlobalLayout() { schedule(); }
    @Override public void onWindowFocusChanged(boolean focused) { if (focused) schedule(); }

    static final class Editors {
        interface Callback {
            String edit(long token,String text);
            boolean focus(long token,boolean focused);
            void selection(long token,int start,int end);
            void submit(long token);
            void scroll(long token,float x,float y,float width,float height);
            void alpha(long token,View view,float alpha);
            void detached(long token);
        }
        private final android.app.Activity activity;
        private final LabelOverlay overlay;
        private final Callback callback;
        private final java.util.HashMap<Long,Entry> entries=new java.util.HashMap<>();
        Editors(android.app.Activity activity,LabelOverlay overlay,Callback callback){this.activity=activity;this.overlay=overlay;this.callback=callback;}
        View view(long token){Entry entry=entries.get(token);return entry==null ? null : entry.editor;}
        void sync(long token,LabelOverlay parent,CharSequence text,String hint,float[] metrics,int[] options){Entry entry=entries.get(token);if(entry==null){if(options[1]==0)return;entry=new Entry(token);entries.put(token,entry);}entry.sync(parent,text,hint,metrics,options);}
        boolean focus(long token,boolean focused){Entry entry=entries.get(token);if(entry==null)return false;entry.updating=true;try{if(focused){if(entry.editable)entry.ime.requestShow();else entry.editor.requestFocus();}else{entry.ime.cancel();((InputMethodManager)activity.getSystemService(android.content.Context.INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(entry.editor.getWindowToken(),0);entry.editor.clearFocus();}}finally{entry.updating=false;}return true;}
        void remove(long token){Entry entry=entries.remove(token);if(entry!=null)entry.close();}
        void close(){for(Entry entry:entries.values())entry.close();entries.clear();}
        private final class Entry {
            final long token;
            final Editor editor;
            final TextInput ime;
            android.text.method.KeyListener keyListener;
            boolean updating,editable=true,selectable=true,scrolling=true,directional;
            float scale=1,insetX,insetY,downX,downY;
            int anchorX,anchorY,axis,configuredType=-1;
            Entry(long token){
                this.token=token;editor=new Editor(activity);editor.entry=this;
                editor.setSingleLine(false);editor.setIncludeFontPadding(false);editor.setBackgroundColor(android.graphics.Color.TRANSPARENT);editor.setGravity(android.view.Gravity.TOP|android.view.Gravity.START);
                ime=new TextInput(editor);keyListener=editor.getKeyListener();
                editor.addTextChangedListener(new android.text.TextWatcher(){
                    public void beforeTextChanged(CharSequence text,int start,int count,int after){}
                    public void onTextChanged(CharSequence text,int start,int before,int count){}
                    public void afterTextChanged(android.text.Editable text){if(updating || !editable)return;String accepted=callback.edit(token,text.toString());if(accepted!=null && !accepted.contentEquals(text)){updating=true;try{editor.setText(accepted);editor.setSelection(Math.min(accepted.length(),Math.max(0,editor.getSelectionStart())));}finally{updating=false;}}selection();}
                });
                editor.setOnFocusChangeListener((view,focused)->{if(updating)return;if(!callback.focus(token,focused)){updating=true;try{if(focused)editor.clearFocus();else editor.requestFocus();}finally{updating=false;}}});
                editor.setOnEditorActionListener((view,action,event)->{if(entrySingle && (event==null || event.getAction()==android.view.KeyEvent.ACTION_UP))callback.submit(token);return entrySingle;});
            }
            boolean entrySingle;
            void sync(LabelOverlay parent,CharSequence text,String hint,float[] m,int[] o){
                updating=true;
                try{
                    scale=m[18];insetX=m[19];insetY=m[20];editable=o[3]!=0;selectable=o[4]!=0;scrolling=o[17]!=0;directional=o[20]!=0;
                    entrySingle=o[22]!=0;
                    if(o[1]!=0)LabelOverlay.attach(parent,editor);
                    else if(editor.getParent()!=null){ime.cancel();LabelOverlay.detach(editor);callback.detached(token);}
                    int width=Math.max(0,(int)Math.ceil(m[2])),height=Math.max(0,(int)Math.ceil(m[3]));android.view.ViewGroup.LayoutParams old=editor.getLayoutParams();
                    if(old==null || old.width!=width || old.height!=height)editor.setLayoutParams(new android.widget.FrameLayout.LayoutParams(width,height));
                    editor.setTranslationX(m[0]);editor.setTranslationY(m[1]);parent.depth(editor,m[5],m[6]);
                    editor.setClipBounds(new android.graphics.Rect((int)m[7],(int)m[8],(int)Math.ceil(m[7]+m[9]),(int)Math.ceil(m[8]+m[10])));
                    editor.setPadding(Math.round(m[12]),Math.round(m[13]),Math.round(m[14]),Math.round(m[15]));editor.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX,m[11]);
                    int type=TextInput.inputType(o[7],o[9],o[10]!=0);
                    if((type&InputType.TYPE_MASK_CLASS)==InputType.TYPE_CLASS_TEXT){if(!entrySingle)type|=InputType.TYPE_TEXT_FLAG_MULTI_LINE;if(o[5]==1)type|=InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;else type|=InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;if(o[6]==1)type|=InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;}
                    if(configuredType!=type){editor.setInputType(type);keyListener=editor.getKeyListener();configuredType=type;}
                    editor.setSingleLine(entrySingle);editor.setGravity((entrySingle ? android.view.Gravity.CENTER_VERTICAL : android.view.Gravity.TOP)|android.view.Gravity.START);editor.setHint(hint);
                    editor.setTextIsSelectable(selectable && !editable);editor.setKeyListener(editable ? keyListener : null);editor.setCursorVisible(editable);editor.setShowSoftInputOnFocus(editable);
                    editor.setImeOptions(o[8]==0 ? EditorInfo.IME_ACTION_NONE|EditorInfo.IME_FLAG_NO_FULLSCREEN|EditorInfo.IME_FLAG_NO_EXTRACT_UI : TextInput.options(o[8]));
                    editor.setTextColor(o[13]);editor.setBackgroundColor(o[14]);editor.setTextAlignment(o[12]==1 ? View.TEXT_ALIGNMENT_CENTER : o[12]==2 ? View.TEXT_ALIGNMENT_TEXT_END : View.TEXT_ALIGNMENT_TEXT_START);
                    int links=editable ? 0 : ((o[11]&1)!=0 ? android.text.util.Linkify.PHONE_NUMBERS : 0)|((o[11]&2)!=0 ? android.text.util.Linkify.WEB_URLS|android.text.util.Linkify.EMAIL_ADDRESSES : 0)|((o[11]&4)!=0 ? android.text.util.Linkify.MAP_ADDRESSES : 0);
                    if(text!=null){editor.setText(text);if(links!=0)android.text.util.Linkify.addLinks(editor.getText(),links);}
                    if(!editable && selectable && links!=0)editor.setMovementMethod(android.text.method.LinkMovementMethod.getInstance());else if(selectable || editable)editor.setMovementMethod(android.text.method.ArrowKeyMovementMethod.getInstance());else editor.setMovementMethod(null);
                    int start=Math.min(editor.length(),Math.max(0,o[15])),end=Math.min(editor.length(),Math.max(start,o[16]));if(editor.getSelectionStart()!=start || editor.getSelectionEnd()!=end)editor.setSelection(start,end);
                    editor.setVerticalScrollBarEnabled(o[18]!=0);editor.setHorizontalScrollBarEnabled(o[19]!=0);editor.scrollTo(Math.round(m[16]),Math.round(m[17]));
                    editor.setEnabled(o[2]!=0);editor.setVisibility(o[0]!=0 ? View.VISIBLE : View.GONE);parent.visibility(editor,o[0]!=0 && m[4]>0);
                    if(o[1]!=0)callback.alpha(token,editor,m[4]);
                }finally{updating=false;}
            }
            void selection(){if(!updating && editor.getSelectionStart()>=0)callback.selection(token,Math.min(editor.getSelectionStart(),editor.getSelectionEnd()),Math.max(editor.getSelectionStart(),editor.getSelectionEnd()));}
            void scroll(){if(!updating && editor.getLayout()!=null && scale>0)callback.scroll(token,(editor.getScrollX()-insetX)/scale,(editor.getScrollY()-insetY)/scale,editor.contentWidth()/scale,editor.contentHeight()/scale);}
            void close(){updating=true;ime.cancel();editor.clearFocus();editor.entry=null;LabelOverlay.detach(editor);callback.detached(token);}
        }
        private final class Editor extends EditText {
            Entry entry;
            Editor(android.content.Context context){super(context);}
            int contentWidth(){return computeHorizontalScrollRange();}
            int contentHeight(){return computeVerticalScrollRange();}
            @Override protected void onSelectionChanged(int start,int end){super.onSelectionChanged(start,end);if(entry!=null)entry.selection();}
            @Override protected void onLayout(boolean changed,int left,int top,int right,int bottom){super.onLayout(changed,left,top,right,bottom);if(entry!=null)entry.scroll();}
            @Override protected void onScrollChanged(int x,int y,int oldX,int oldY){super.onScrollChanged(x,y,oldX,oldY);if(entry!=null){if(entry.axis==1 && y!=entry.anchorY){scrollTo(x,entry.anchorY);return;}if(entry.axis==2 && x!=entry.anchorX){scrollTo(entry.anchorX,y);return;}entry.scroll();}}
            @Override public boolean onTouchEvent(android.view.MotionEvent event){
                if(entry==null)return false;
                int action=event.getActionMasked();
                if(action==android.view.MotionEvent.ACTION_DOWN){entry.axis=0;entry.downX=event.getX();entry.downY=event.getY();entry.anchorX=getScrollX();entry.anchorY=getScrollY();}
                if(action==android.view.MotionEvent.ACTION_MOVE){if(!entry.scrolling)return true;if(entry.directional && entry.axis==0){float dx=Math.abs(event.getX()-entry.downX),dy=Math.abs(event.getY()-entry.downY);if(Math.max(dx,dy)>=android.view.ViewConfiguration.get(activity).getScaledTouchSlop())entry.axis=dx>dy ? 1 : 2;}}
                boolean handled=super.onTouchEvent(event);if(action==android.view.MotionEvent.ACTION_UP || action==android.view.MotionEvent.ACTION_CANCEL)entry.axis=0;return handled;
            }
        }
    }
}
