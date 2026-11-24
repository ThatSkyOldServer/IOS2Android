package local.ios.host;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AbsListView;
import android.widget.BaseAdapter;
import android.widget.FrameLayout;
import android.widget.ListView;
import java.util.HashMap;
import java.util.HashSet;

final class TableContent {
    interface Callback {
        long[] bind(long table,int position,long previous,float width);
        String event(long table,int event,int position,long row,float top,float width,int state);
        void alpha(long token,View view,float value);
        void detached(long token);
        void failure(String message);
    }
    private final Activity activity;
    private final Callback callback;
    private final int accessoryColor;
    private final HashMap<Long,Entry> tables=new HashMap<>();
    private final HashMap<Long,LabelOverlay> owners=new HashMap<>();
    private boolean closed;

    TableContent(Activity activity,Callback callback){this.activity=activity;this.callback=callback;android.util.TypedValue colour=new android.util.TypedValue();activity.getTheme().resolveAttribute(android.R.attr.colorAccent,colour,true);accessoryColor=colour.data;}
    LabelOverlay overlay(long token){return owners.get(token);}
    View view(long token){Entry entry=tables.get(token);return entry==null ? null : entry.panel;}
    private void event(Entry entry,int event,int position,long row,float top,int state){String failure=callback.event(entry.token,event,position,row,top,entry.list.getWidth(),state);if(failure!=null)callback.failure(failure);}
    void sync(long token,LabelOverlay parent,long background,float[] metrics,int[] options){
        if(closed || activity.isDestroyed())return;
        Entry entry=tables.get(token);
        if(entry==null){if(options[1]==0)return;entry=new Entry(token);tables.put(token,entry);}
        entry.sync(parent,background,metrics,options);
    }
    void appearance(long table,long token,float[] metrics,int[] options){LabelOverlay overlay=owners.get(token);if(overlay instanceof Row)((Row)overlay).appearance(metrics,options);}
    void command(long token,int action,int position,float offset,boolean animated,int mode){
        Entry entry=tables.get(token);if(closed || entry==null)return;
        if(action==4){tables.remove(token);entry.close();return;}
        if(action!=1 && action!=2)return;
        if(position<0 || position>=entry.count)return;
        Runnable scroll=()->{
            if(closed || !tables.containsKey(token))return;
            int top=action==2 ? Math.round(offset)-entry.list.getPaddingTop() : 0;
            if(action==1){
                Row visible=null;for(Row row:entry.rows)if(row.position==position && row.getParent()==entry.list){visible=row;break;}
                if(mode==0 && visible!=null && visible.getTop()>=entry.list.getPaddingTop() && visible.getBottom()<=entry.list.getHeight()-entry.list.getPaddingBottom())return;
                int height=visible!=null ? visible.getHeight() : Math.round(entry.estimatedHeight);
                if(mode==2)top=(entry.list.getHeight()-entry.list.getPaddingTop()-entry.list.getPaddingBottom()-height)/2;
                else if(mode==3 || mode==0 && position>entry.list.getLastVisiblePosition())top=entry.list.getHeight()-entry.list.getPaddingTop()-entry.list.getPaddingBottom()-height;
            }
            if(animated)entry.list.smoothScrollToPositionFromTop(position,top);else entry.list.setSelectionFromTop(position,top);
        };
        if(entry.list.getHeight()==0)entry.list.post(scroll);else scroll.run();
    }
    void close(){closed=true;for(Entry entry:tables.values())entry.close();tables.clear();owners.clear();}

    private final class Entry {
        final long token;
        final LabelOverlay panel,background;
        final ListView list;
        final Rows adapter;
        final HashSet<Row> rows=new HashSet<>();
        long backgroundToken;
        float scale=1,separatorLeft,separatorRight,estimatedHeight=44;
        int count,generation=-1,separatorColor,separatorStyle;
        boolean scrolling=true,selection=true,multiple,closing,attached;
        float[] geometry;
        Entry(long token){
            this.token=token;panel=new LabelOverlay(activity);panel.setClipChildren(true);
            background=new LabelOverlay(activity);panel.addView(background,new FrameLayout.LayoutParams(-1,-1));
            list=new ListView(activity);list.setCacheColorHint(Color.TRANSPARENT);list.setDivider(null);list.setDividerHeight(0);
            list.setSelector(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));list.setClipToPadding(false);
            panel.addView(list,new FrameLayout.LayoutParams(-1,-1));adapter=new Rows(this);list.setAdapter(adapter);
            list.setOnItemClickListener((parent,view,position,id)->{if(selection && view instanceof Row && ((Row)view).kind==0)event(this,0,position,((Row)view).token,0,0);});
            list.setRecyclerListener(view->{if(view instanceof Row)((Row)view).recycle();});
            list.setOnTouchListener((view,touch)->!scrolling && touch.getActionMasked()==MotionEvent.ACTION_MOVE);
            list.setOnScrollListener(new AbsListView.OnScrollListener(){
                public void onScrollStateChanged(AbsListView view,int state){if(!closing)event(Entry.this,4,0,0,0,state);}
                public void onScroll(AbsListView view,int first,int visible,int total){if(!closing && visible>0 && list.getChildCount()>0)event(Entry.this,3,first,0,list.getChildAt(0).getTop(),0);}
            });
        }
        void sync(LabelOverlay parent,long backgroundToken,float[] m,int[] o){
            boolean widthChanged=geometry!=null && geometry[2]!=m[2];
            geometry=m;scale=m[11];selection=o[7]!=0;multiple=o[8]!=0;scrolling=o[11]!=0;separatorColor=o[5];separatorStyle=o[6];separatorLeft=m[16];separatorRight=m[17];estimatedHeight=44*scale;
            if(o[1]==0){attached=false;LabelOverlay.detach(panel);for(Row row:rows.toArray(new Row[0]))row.recycle();callback.detached(token);return;}
            boolean reattach=!attached;attached=true;
            LabelOverlay.attach(parent,panel);
            int width=Math.max(0,(int)Math.ceil(m[2])),height=Math.max(0,(int)Math.ceil(m[3]));ViewGroup.LayoutParams old=panel.getLayoutParams();if(old==null || old.width!=width || old.height!=height)panel.setLayoutParams(new FrameLayout.LayoutParams(width,height));
            panel.setTranslationX(m[0]);panel.setTranslationY(m[1]);parent.depth(panel,m[5],m[6]);
            panel.setClipBounds(new android.graphics.Rect((int)m[7],(int)m[8],(int)Math.ceil(m[7]+m[9]),(int)Math.ceil(m[8]+m[10])));
            panel.setBackgroundColor(o[3]);panel.setVisibility(o[0]!=0 ? View.VISIBLE : View.GONE);parent.visibility(panel,o[0]!=0 && m[4]>0);
            list.setEnabled(o[2]!=0);list.setPadding(Math.round(m[13]),Math.round(m[12]),Math.round(m[15]),Math.round(m[14]));list.setVerticalScrollBarEnabled(o[12]!=0);list.setHorizontalScrollBarEnabled(o[13]!=0);
            panel.visibility(list,o[0]!=0 && m[4]>0);list.setChoiceMode(selection ? multiple ? ListView.CHOICE_MODE_MULTIPLE : ListView.CHOICE_MODE_SINGLE : ListView.CHOICE_MODE_NONE);
            if(this.backgroundToken!=backgroundToken){if(owners.get(this.backgroundToken)==background)owners.remove(this.backgroundToken);background.removeAllViews();this.backgroundToken=backgroundToken;if(backgroundToken!=0)owners.put(backgroundToken,background);}
            if(reattach || widthChanged || count!=o[4] || generation!=o[9]){count=o[4];generation=o[9];adapter.notifyDataSetChanged();}
            else for(Row row:rows)row.invalidate();
            callback.alpha(token,panel,m[4]);
        }
        void close(){closing=true;for(Row row:rows.toArray(new Row[0]))row.recycle();rows.clear();list.setOnScrollListener(null);list.setAdapter(null);if(owners.get(backgroundToken)==background)owners.remove(backgroundToken);LabelOverlay.detach(panel);background.removeAllViews();callback.detached(token);}
    }
    private final class Rows extends BaseAdapter {
        final Entry entry;
        Rows(Entry entry){this.entry=entry;}
        public int getCount(){return entry.count;}
        public Object getItem(int position){return position;}
        public long getItemId(int position){return position;}
        @Override public boolean hasStableIds(){return false;}
        @Override public boolean areAllItemsEnabled(){return false;}
        @Override public boolean isEnabled(int position){return entry.selection;}
        public View getView(int position,View convert,ViewGroup parent){
            Row row=convert instanceof Row && ((Row)convert).entry==entry ? (Row)convert : new Row(entry);
            long previous=row.token;if(owners.get(previous)==row)owners.remove(previous);
            float width=Math.max(0,parent.getWidth()-parent.getPaddingLeft()-parent.getPaddingRight());if(width==0 && entry.geometry!=null)width=Math.max(0,entry.geometry[2]-entry.geometry[13]-entry.geometry[15]);
            long[] binding=callback.bind(entry.token,position,previous,width);
            if(binding==null || binding.length<6){row.token=0;row.removeAllViews();row.setLayoutParams(new AbsListView.LayoutParams(-1,0));return row;}
            LabelOverlay former=owners.get(binding[0]);if(former instanceof Row && former!=row){((Row)former).token=0;entry.rows.remove((Row)former);}
            row.token=binding[0];row.position=position;row.kind=(int)binding[2];row.accessory=(int)binding[3];row.selectionStyle=(int)binding[4];row.selected=binding[5]!=0;owners.put(row.token,row);entry.rows.add(row);
            row.setLayoutParams(new AbsListView.LayoutParams(-1,Math.max(0,(int)Math.ceil(Float.intBitsToFloat((int)binding[1])))));if(row.isAttachedToWindow())event(entry,5,position,row.token,0,0);row.invalidate();return row;
        }
    }
    private final class Row extends LabelOverlay {
        final Entry entry;
        final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        long token;
        int position,kind,accessory,selectionStyle;
        boolean selected,accessoryPressed,separatorOverride;
        float separatorLeft,separatorRight;
        Row(Entry entry){super(activity);this.entry=entry;setClipChildren(true);setClipToPadding(true);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);}
        void appearance(float[] metrics,int[] options){accessory=options[0];selectionStyle=options[1];selected=options[2]!=0;separatorOverride=options[3]!=0;separatorLeft=metrics[0];separatorRight=metrics[1];invalidate();}
        void recycle(){if(token==0)return;long previous=token;token=0;if(owners.get(previous)==this)owners.remove(previous);entry.rows.remove(this);event(entry,2,position,previous,0,0);removeAllViews();}
        @Override protected void onAttachedToWindow(){super.onAttachedToWindow();if(token!=0)event(entry,5,position,token,0,0);}
        @Override protected void onDetachedFromWindow(){if(token!=0 && !entry.closing)event(entry,6,position,token,0,0);super.onDetachedFromWindow();}
        @Override public void onStartTemporaryDetach(){if(token!=0 && !entry.closing)event(entry,6,position,token,0,0);super.onStartTemporaryDetach();}
        @Override public void onFinishTemporaryDetach(){super.onFinishTemporaryDetach();if(token!=0)event(entry,5,position,token,0,0);}
        @Override public boolean onTouchEvent(MotionEvent touch){
            if(kind==0 && (accessory==2 || accessory==4)){
                boolean inside=touch.getX()>=getWidth()-36*entry.scale;
                if(touch.getActionMasked()==MotionEvent.ACTION_DOWN && inside){accessoryPressed=true;return true;}
                if(accessoryPressed){if(touch.getActionMasked()==MotionEvent.ACTION_UP){accessoryPressed=false;if(inside)event(entry,1,position,token,0,0);}else if(touch.getActionMasked()==MotionEvent.ACTION_CANCEL)accessoryPressed=false;return true;}
            }
            return super.onTouchEvent(touch);
        }
        @Override protected void dispatchDraw(Canvas canvas){
            super.dispatchDraw(canvas);float scale=entry.scale;
            if(kind==0 && entry.separatorStyle!=0){paint.setStyle(Paint.Style.FILL);paint.setColor(entry.separatorColor);float left=separatorOverride ? separatorLeft : entry.separatorLeft,right=separatorOverride ? separatorRight : entry.separatorRight;canvas.drawRect(Math.max(0,left),getHeight()-1,Math.max(0,getWidth()-right),getHeight(),paint);}
            if(kind!=0 || accessory==0)return;
            paint.setColor(accessory==1 ? 0xff8e8e93 : accessoryColor);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(Math.max(1,1.5f*scale));float x=getWidth()-22*scale,y=getHeight()*0.5f;
            if(accessory==1 || accessory==2){canvas.drawLine(x-3*scale,y-5*scale,x+2*scale,y,paint);canvas.drawLine(x+2*scale,y,x-3*scale,y+5*scale,paint);}
            if(accessory==3){canvas.drawLine(x-6*scale,y,x-2*scale,y+4*scale,paint);canvas.drawLine(x-2*scale,y+4*scale,x+6*scale,y-5*scale,paint);}
            if(accessory==2 || accessory==4){if(accessory==2)x-=20*scale;canvas.drawCircle(x,y,8*scale,paint);canvas.drawLine(x,y-1*scale,x,y+4*scale,paint);paint.setStyle(Paint.Style.FILL);canvas.drawCircle(x,y-4*scale,scale,paint);}
        }
    }
}
