package local.ios.host;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Region;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import java.util.IdentityHashMap;

class LabelOverlay extends FrameLayout {
    static void detach(View child) {
        if(child.getParent() instanceof android.view.ViewGroup)((android.view.ViewGroup)child.getParent()).removeView(child);
    }
    static void attach(LabelOverlay parent,View child) {
        if(child.getParent()==parent)return;
        detach(child);parent.addView(child);
    }
    static void updateVisibility(View child,boolean visible) {
        if(child.getParent() instanceof LabelOverlay)((LabelOverlay)child.getParent()).visibility(child,visible);
    }
    private static final class Entry {
        final View view;
        float depth, windowLevel;
        int childIndex, position;
        boolean visible;
        Matrix transform,inverse,world;
        boolean invertible=true;
        float[] clipPlanes;
        Matrix clipMatrix;
        RectF clipBounds;
        Region clipRegion;
        Path[] clips;
        Region[] hitClips;
        final float[] values=new float[9];
        Entry(View view) { this.view=view; }
    }
    private final IdentityHashMap<View,Entry> entries=new IdentityHashMap<>();
    private Entry[] order;
    private boolean dirty;
    private int visibleCount;
    private int transformedCount;
    private View touchTarget;
    private final int[] windowLocation=new int[2];
    void visibility(View child,boolean visible) {
        Entry entry=entries.get(child);
        if (entry==null || entry.visible==visible) return;
        boolean hadVisible=visibleCount!=0;
        entry.visible=visible;
        visibleCount+=visible ? 1 : -1;
        if (hadVisible!=(visibleCount!=0) && getParent()!=null)
            getParent().requestTransparentRegion(this);
    }
    @Override public boolean gatherTransparentRegion(android.graphics.Region region) {

        if (visibleCount!=0 && getVisibility()==View.VISIBLE) {
            if (region==null) return false;
            getLocationInWindow(windowLocation);
            region.op(windowLocation[0],windowLocation[1],windowLocation[0]+getWidth(),
                    windowLocation[1]+getHeight(),android.graphics.Region.Op.DIFFERENCE);
            return false;
        }
        return true;
    }

    LabelOverlay(Context context) {
        super(context);
        setChildrenDrawingOrderEnabled(true);
        setClipChildren(false); setClipToPadding(false);
    }
    @Override public void onViewAdded(View child) {
        super.onViewAdded(child);
        entries.put(child,new Entry(child)); order=null; dirty=true;
    }
    @Override public void onViewRemoved(View child) {
        super.onViewRemoved(child);
        Entry entry=entries.remove(child);
        if(entry!=null && entry.transform!=null)--transformedCount;
        if(touchTarget==child){long now=android.os.SystemClock.uptimeMillis();MotionEvent cancel=MotionEvent.obtain(now,now,MotionEvent.ACTION_CANCEL,0,0,0);child.dispatchTouchEvent(cancel);cancel.recycle();touchTarget=null;}
        if (entry!=null && entry.visible && --visibleCount==0 && getParent()!=null)getParent().requestTransparentRegion(this);
        order=null; dirty=true;
    }
    @Override public void bringChildToFront(View child) {
        super.bringChildToFront(child); order=null; dirty=true;
    }
    private static int compare(Entry first,Entry second) {
        int window=Float.compare(first.windowLevel,second.windowLevel);
        if (window!=0) return window;
        int depth=Float.compare(first.depth,second.depth);
        return depth!=0 ? depth : Integer.compare(first.childIndex,second.childIndex);
    }
    void depth(View child,float value,float windowLevel) {
        Entry entry=entries.get(child);
        if (entry==null || entry.depth==value && entry.windowLevel==windowLevel) return;
        entry.depth=value; entry.windowLevel=windowLevel;
        if (order==null || dirty) { dirty=true; invalidate(); return; }
        int position=entry.position;
        if ((position>0 && compare(order[position-1],entry)>0) ||
                (position+1<order.length && compare(entry,order[position+1])>0)) {
            dirty=true; invalidate();
        }
    }
    void composition(View child,float[] presentation,float[] clipPlanes,boolean clippingChanged) {
        Entry entry=entries.get(child);if(entry==null)return;
        float[] p=presentation;boolean plain=p[11]==1 && p[12]==0 && p[13]==p[0] && p[14]==0 && p[15]==1 && p[16]==p[1] && p[17]==0 && p[18]==0 && p[19]==1;
        if(plain){if(entry.transform!=null){entry.transform=null;entry.inverse=null;--transformedCount;}}
        else {if(entry.transform==null){entry.transform=new Matrix();entry.inverse=new Matrix();entry.world=new Matrix();++transformedCount;}System.arraycopy(p,11,entry.values,0,9);entry.transform.setValues(entry.values);entry.transform.preTranslate(-p[0]-child.getLeft(),-p[1]-child.getTop());
            entry.world.setValues(entry.values);entry.invertible=entry.world.invert(entry.inverse);}
        if(entry.clipPlanes==clipPlanes && !clippingChanged){invalidate();return;}entry.clipPlanes=clipPlanes;
        int count=clipPlanes==null ? 0 : clipPlanes.length/12;
        if(count==0){entry.clips=null;entry.hitClips=null;}else if(entry.clips==null || entry.clips.length!=count){entry.clips=new Path[count];entry.hitClips=new Region[count];for(int i=0;i<count;i++){entry.clips[i]=new Path();entry.hitClips[i]=new Region();}}
        if(count>0 && entry.clipMatrix==null){entry.clipMatrix=new Matrix();entry.clipBounds=new RectF();entry.clipRegion=new Region();}
        for(int i=0;i<count;i++){int offset=i*12;Path path=entry.clips[i];path.reset();float width=clipPlanes[offset],height=clipPlanes[offset+1],radius=clipPlanes[offset+2];entry.clipBounds.set(0,0,width,height);path.addRoundRect(entry.clipBounds,radius,radius,Path.Direction.CW);
            System.arraycopy(clipPlanes,offset+3,entry.values,0,9);entry.clipMatrix.setValues(entry.values);path.transform(entry.clipMatrix);path.computeBounds(entry.clipBounds,true);entry.clipRegion.set((int)Math.floor(entry.clipBounds.left),(int)Math.floor(entry.clipBounds.top),(int)Math.ceil(entry.clipBounds.right),(int)Math.ceil(entry.clipBounds.bottom));entry.hitClips[i].setPath(path,entry.clipRegion);}
        invalidate();
    }
    @Override protected boolean drawChild(Canvas canvas,View child,long time) {
        Entry entry=entries.get(child);if(entry==null || entry.transform==null && entry.clips==null)return super.drawChild(canvas,child,time);
        int saved=canvas.save();if(entry.clips!=null)for(Path clip:entry.clips)canvas.clipPath(clip);if(entry.transform!=null)canvas.concat(entry.transform);
        boolean drawn=super.drawChild(canvas,child,time);canvas.restoreToCount(saved);return drawn;
    }
    private boolean dispatchChild(View child,MotionEvent event,boolean hit) {
        Entry entry=entries.get(child);if(entry==null)return false;
        if(entry.transform!=null && !entry.invertible)return false;
        if(hit && entry.hitClips!=null)for(Region region:entry.hitClips)if(!region.contains((int)event.getX(),(int)event.getY()))return false;
        MotionEvent local=MotionEvent.obtain(event);
        if(entry.transform!=null)local.transform(entry.inverse);
        else {local.offsetLocation(getScrollX()-child.getLeft(),getScrollY()-child.getTop());if(!child.getMatrix().isIdentity()){Matrix inverse=new Matrix();if(!child.getMatrix().invert(inverse)){local.recycle();return false;}local.transform(inverse);}}
        if(hit && (local.getX()<0 || local.getY()<0 || local.getX()>=child.getWidth() || local.getY()>=child.getHeight())){local.recycle();return false;}
        boolean handled=child.dispatchTouchEvent(local);local.recycle();return handled;
    }
    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        int action=event.getActionMasked();if(touchTarget!=null){View target=touchTarget;boolean handled=dispatchChild(target,event,false);if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_CANCEL)touchTarget=null;return handled;}
        if(transformedCount==0 || action!=MotionEvent.ACTION_DOWN)return super.dispatchTouchEvent(event);
        for(int i=getChildCount()-1;i>=0;--i){View child=getChildAt(getChildDrawingOrder(getChildCount(),i));if(child.getVisibility()!=View.VISIBLE || child.getAlpha()<=0 || !child.isEnabled())continue;if(dispatchChild(child,event,true)){touchTarget=child;return true;}}
        return onTouchEvent(event);
    }
    @Override protected int getChildDrawingOrder(int count,int drawingPosition) {
        if (order==null || order.length!=count) {
            order=new Entry[count];
            for (int i=0;i<count;++i) {
                Entry entry=entries.get(getChildAt(i)); entry.childIndex=i; order[i]=entry;
            }
            dirty=true;
        }
        if (dirty) {

            for (int i=1;i<count;++i) {
                Entry entry=order[i]; int j=i;
                while (j>0 && compare(order[j-1],entry)>0) { order[j]=order[j-1]; --j; }
                order[j]=entry;
            }
            for (int i=0;i<count;++i) order[i].position=i;
            dirty=false;
        }
        return order[drawingPosition].childIndex;
    }
}
