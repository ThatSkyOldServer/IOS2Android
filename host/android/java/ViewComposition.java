package local.ios.host;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.LinearInterpolator;
import android.widget.FrameLayout;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;

final class ViewComposition {
    private static final int PRESENTATION_FIELDS=25;
    private static final android.animation.TimeInterpolator[] CURVES={new android.view.animation.PathInterpolator(.42f,0,.58f,1),new android.view.animation.PathInterpolator(.42f,0,1,1),new android.view.animation.PathInterpolator(0,0,.58f,1),new LinearInterpolator()};
    interface Callback {
        LabelOverlay overlay(long owner);
        View view(long token);
        String event(long token,int event,boolean completed);
        void failure(String message);
    }
    private final Context context;
    private final Callback callback;
    private final HashMap<Long,Node> nodes=new HashMap<>();
    private final HashMap<Long,AlphaChannel> alphaChannels=new HashMap<>();
    private final Set<Animation> animations=new HashSet<>();
    private boolean closed;
    ViewComposition(Context context,Callback callback) { this.context=context;this.callback=callback; }
    private Node node(long token) { Node node=nodes.get(token);if(node==null){node=new Node(token);nodes.put(token,node);}return node; }
    private void event(long token,int event,boolean completed) { if(token!=0){String failure=callback.event(token,event,completed);if(failure!=null)callback.failure(failure);} }
    void sync(long token,long owner,boolean raw,float[] presentation,float[] clips,long[] clipOwners) {
        if(closed)return;if(presentation[20]==0){remove(token);return;}Node node=node(token);node.raw=raw;node.owner=owner;node.model=presentation;node.clips=clips==null ? null : clips.clone();node.clipSources=clips==null ? null : new Clip[clipOwners.length];node.clipsChanged=true;
        if(clips!=null)for(int i=0;i<clipOwners.length;i++)node.clipSources[i]=new Clip(node(clipOwners[i]),Arrays.copyOfRange(clips,i*12,i*12+12));
        if(raw && node.view==null && presentation[22]!=0){View view=new View(context);view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);node.view=view;}
        if(node.view==null)node.view=callback.view(token);
        if(node.geometry==null)System.arraycopy(presentation,0,node.live,0,PRESENTATION_FIELDS);
        node.bind();node.render();
    }
    void layer(long token,long owner,boolean raw,boolean replace,float[] metrics,int[] options,int[] colors,float[] locations,byte[] image) {
        if(closed)return;Node node=node(token);node.raw=raw;node.owner=owner;Bitmap bitmap=replace ? image==null ? null : BitmapFactory.decodeByteArray(image,0,image.length) : node.layer==null ? null : node.layer.image;if(replace && image!=null && bitmap==null)throw new IllegalArgumentException("Unable to decode the supplied layer image.");node.layer=new Layer(metrics,options,colors,locations,bitmap);
        if(node.view==null)node.view=callback.view(token);node.bind();node.render();
    }
    void bind(long token,View view) { if(closed)return;Node node=node(token);node.view=view;node.bind();node.render(); }
    void alpha(long token,View view,float value) { if(closed)return;Node node=node(token);node.view=view;node.bind();if(node.model==null)view.setAlpha(value);else node.render(); }
    View view(long token) { Node node=nodes.get(token);return node==null ? null : node.view; }
    void remove(long token) {
        Node node=nodes.remove(token);if(node==null)return;Set<Animation> interrupted=new HashSet<>();AlphaChannel alpha=alphaChannels.get(token);if(alpha!=null)interrupted.add(alpha.animation);if(node.geometry!=null)interrupted.add(node.geometry.animation);
        for(Animation animation:interrupted)animation.animator.cancel();if(node.raw && node.view!=null)LabelOverlay.detach(node.view);if(!node.raw && node.view!=null){if(node.background!=null && node.view.getBackground()==node.background)node.view.setBackground(node.background.fallback);if(node.foreground!=null && node.view.getForeground()==node.foreground)node.view.setForeground(node.foreground.fallback);}node.view=null;node.layer=null;node.opacity=null;
    }
    void close() { if(closed)return;closed=true;for(Animation animation:new ArrayList<>(animations))animation.animator.cancel();for(Node node:nodes.values())if(node.raw && node.view!=null)LabelOverlay.detach(node.view);nodes.clear();alphaChannels.clear(); }
    private final class Node {
        final long token;final float[] live=new float[PRESENTATION_FIELDS],draw=new float[PRESENTATION_FIELDS];
        long owner;boolean raw,clipsChanged;View view;float[] model,clips;Clip[] clipSources;Layer layer;Decoration background,foreground;Opacity opacity;Geometry geometry;
        Node(long token) { this.token=token; }
        void bind() {
            if(view==null)return;LabelOverlay parent=callback.overlay(owner);if(parent!=null)LabelOverlay.attach(parent,view);
            if(layer!=null){if(background==null){background=new Decoration(false);foreground=new Decoration(true);}Drawable previous=view.getBackground();if(previous!=background){background.fallback=previous;view.setBackground(background);}previous=view.getForeground();if(previous!=foreground){foreground.fallback=previous;view.setForeground(foreground);}if(background.layer!=layer){background.layer=foreground.layer=layer;background.shaderWidth=foreground.shaderWidth=-1;view.invalidate();}}
        }
        void render() {
            if(model==null)return;
            float[] p=geometry==null ? model : live;float alpha=opacity==null ? model[4] : opacity.alpha();
            if(geometry!=null){System.arraycopy(p,0,draw,0,PRESENTATION_FIELDS);float sx=geometry.after[2]>0 ? p[2]/geometry.after[2] : 1,sy=geometry.after[3]>0 ? p[3]/geometry.after[3] : 1;draw[11]*=sx;draw[14]*=sx;draw[17]*=sx;draw[12]*=sy;draw[15]*=sy;draw[18]*=sy;p=draw;}
            if(view==null)return;
            int width=Math.max(0,(int)Math.ceil(model[2])),height=Math.max(0,(int)Math.ceil(model[3]));ViewGroup.LayoutParams previous=view.getLayoutParams();if(previous==null || previous.width!=width || previous.height!=height){if(previous==null)previous=new FrameLayout.LayoutParams(width,height);else {previous.width=width;previous.height=height;}view.setLayoutParams(previous);}
            view.setTranslationX(p[0]);view.setTranslationY(p[1]);view.setAlpha(Math.max(0,Math.min(1,alpha)));view.setClipBounds(null);boolean visible=model[20]!=0 && model[21]!=0 && alpha>0 && p[2]>0 && p[3]>0;view.setVisibility(visible ? View.VISIBLE : View.GONE);
            if(clipSources!=null)for(int i=0;i<clipSources.length;i++)clipsChanged|=clipSources[i].update(clips,i*12);
            if(view.getParent() instanceof LabelOverlay){LabelOverlay parent=(LabelOverlay)view.getParent();parent.depth(view,p[5],p[6]);parent.composition(view,p,clips,clipsChanged);parent.visibility(view,visible);clipsChanged=false;}
        }
        void renderAlpha() {
            if(view==null || model==null)return;float alpha=opacity==null ? model[4] : opacity.alpha();view.setAlpha(Math.max(0,Math.min(1,alpha)));boolean visible=model[20]!=0 && model[21]!=0 && alpha>0 && model[2]>0 && model[3]>0;view.setVisibility(visible ? View.VISIBLE : View.GONE);LabelOverlay.updateVisibility(view,visible);
        }
    }
    private final class Clip {
        final Node owner;final float[] original;
        Clip(Node owner,float[] original){this.owner=owner;this.original=original;}
        boolean update(float[] target,int offset){float[] live=owner.geometry==null ? null : owner.live;boolean changed=false;for(int i=0;i<12;i++){float value=live==null ? original[i] : i==0 ? live[2] : i==1 ? live[3] : i==2 ? original[2] : live[i+8];if(target[offset+i]!=value){target[offset+i]=value;changed=true;}}return changed;}
    }
    private final class AlphaChannel {
        final long token;final float before,after;final Animation animation;float current;
        AlphaChannel(long token,float before,float after,Animation animation){this.token=token;this.before=before;this.after=after;this.animation=animation;current=before;}
    }
    private final class Opacity {
        final long[] ancestors;final float[] model;AlphaChannel[] factors;float base;
        Opacity(long[] ancestors,float[] model){this.ancestors=ancestors;this.model=model;bind();}
        void bind(){base=1;ArrayList<AlphaChannel> active=new ArrayList<>();for(int i=0;i<ancestors.length;i++){AlphaChannel channel=alphaChannels.get(ancestors[i]);if(channel==null)base*=model[i];else active.add(channel);}factors=active.toArray(new AlphaChannel[0]);}
        float alpha(){float value=base;for(AlphaChannel channel:factors)value*=channel.current;return value;}
    }
    private final class Geometry {
        final Node node;final Animation animation;final float[] before,after;
        final boolean affine;final double[] from,to;final double rotation;
        Geometry(Node node,Animation animation,float[] before,float[] after){this.node=node;this.animation=animation;this.before=before;this.after=after;affine=before[17]==0 && before[18]==0 && before[19]==1 && after[17]==0 && after[18]==0 && after[19]==1 && Math.hypot(before[11],before[14])>1e-8 && Math.hypot(after[11],after[14])>1e-8;from=affine ? decompose(before) : null;to=affine ? decompose(after) : null;double angle=affine ? to[0]-from[0] : 0;rotation=Math.atan2(Math.sin(angle),Math.cos(angle));}
        double[] decompose(float[] p){double a=p[11],b=p[14],c=p[12],d=p[15],sx=Math.hypot(a,b);return new double[]{Math.atan2(b,a),sx,(a*d-b*c)/sx,(a*c+b*d)/sx,p[13]+a*p[2]*p[23]+c*p[3]*p[24],p[16]+b*p[2]*p[23]+d*p[3]*p[24]};}
        void update(float fraction){float[] p=node.live;for(int i=0;i<PRESENTATION_FIELDS;i++)p[i]=before[i]+(after[i]-before[i])*fraction;
            if(affine){double angle=from[0]+rotation*fraction,sine=Math.sin(angle),cosine=Math.cos(angle),sx=from[1]+(to[1]-from[1])*fraction,sy=from[2]+(to[2]-from[2])*fraction,shear=from[3]+(to[3]-from[3])*fraction;p[11]=(float)(cosine*sx);p[14]=(float)(sine*sx);p[12]=(float)(cosine*shear-sine*sy);p[15]=(float)(sine*shear+cosine*sy);p[13]=(float)(from[4]+(to[4]-from[4])*fraction-p[11]*p[2]*p[23]-p[12]*p[3]*p[24]);p[16]=(float)(from[5]+(to[5]-from[5])*fraction-p[14]*p[2]*p[23]-p[15]*p[3]*p[24]);}
            }
    }
    private static boolean geometryChanged(float[] before,float[] after){for(int i=0;i<20;i++)if(i!=4 && before[i]!=after[i])return true;return false;}
    private final class Animation {
        final ValueAnimator animator=ValueAnimator.ofFloat(0,1);final ArrayList<AlphaChannel> channels=new ArrayList<>();final ArrayList<Geometry> geometry=new ArrayList<>();final Set<Node> affected=new LinkedHashSet<>();final long completion;final int options;final float damping,velocity;boolean ended,started,timeline;float cycles=1;
        Animation(long completion,int options,float damping,float velocity){this.completion=completion;this.options=options;this.damping=damping;this.velocity=velocity;
            animator.setInterpolator(new LinearInterpolator());animator.addUpdateListener(value->{float position=(Float)value.getAnimatedValue();if(timeline){int cycle=(int)Math.floor(position);position-=cycle;if(position==0 && cycle>0){--cycle;position=1;}if((options&16)!=0 && (cycle&1)!=0)position=1-position;}float fraction=curve(position);for(AlphaChannel channel:channels)channel.current=channel.before+(channel.after-channel.before)*fraction;for(Geometry item:geometry)item.update(fraction);for(Node node:affected)if(geometry.isEmpty())node.renderAlpha();else node.render();});
            animator.addListener(new AnimatorListenerAdapter(){@Override public void onAnimationStart(Animator animator){start();}@Override public void onAnimationCancel(Animator animator){finish(false);}@Override public void onAnimationEnd(Animator animator){finish(true);}});
        }
        void start(){if(started || ended)return;started=true;event(completion,0,true);}
        float curve(float fraction){
            if(damping>=0){double omega=damping==0 ? Math.PI*2.5 : 8/Math.max(.05,damping),z=damping,t=fraction;
                double value,end;if(z<1){double frequency=omega*Math.sqrt(1-z*z),coefficient=(z*omega-velocity)/frequency;value=1-Math.exp(-z*omega*t)*(Math.cos(frequency*t)+coefficient*Math.sin(frequency*t));end=1-Math.exp(-z*omega)*(Math.cos(frequency)+coefficient*Math.sin(frequency));}
                else {value=1-Math.exp(-omega*t)*(1+(omega-velocity)*t);end=1-Math.exp(-omega)*(1+omega-velocity);}return fraction>=1 ? 1 : fraction<=0 ? 0 : (float)(Math.abs(end)>1e-6 ? value/end : value);}
            return CURVES[(options>>16)&3].getInterpolation(fraction);
        }
        void render(){for(Geometry item:geometry)item.update(0);for(Node node:affected)if(geometry.isEmpty())node.renderAlpha();else node.render();}
        void finish(boolean completed){if(ended)return;ended=true;for(AlphaChannel channel:channels)if(alphaChannels.get(channel.token)==channel)alphaChannels.remove(channel.token);for(Geometry item:geometry)if(item.node.geometry==item)item.node.geometry=null;
            for(Node node:affected){if(node.opacity!=null){node.opacity.bind();if(node.opacity.factors.length==0)node.opacity=null;}node.render();}animations.remove(this);event(completion,1,completed);}
    }
    void animate(long[] changed,float[] before,float[] after,long[] tokens,int[] offsets,long[] ancestors,float[] model,float[] framesBefore,float[] framesAfter,long duration,long delay,int options,float damping,float velocity,float repeat,long completion,long requested) {
        if(closed){event(completion,1,false);return;}Set<Animation> interrupted=new HashSet<>();
        for(int i=0;i<changed.length;i++){if(before[i]==after[i])continue;AlphaChannel old=alphaChannels.get(changed[i]);if(old!=null){if((options&4)!=0)before[i]=old.current;interrupted.add(old.animation);}}
        for(int i=0;i<tokens.length;i++){Node node=nodes.get(tokens[i]);if(node!=null && node.geometry!=null && geometryChanged(Arrays.copyOfRange(framesBefore,i*PRESENTATION_FIELDS,i*PRESENTATION_FIELDS+PRESENTATION_FIELDS),Arrays.copyOfRange(framesAfter,i*PRESENTATION_FIELDS,i*PRESENTATION_FIELDS+PRESENTATION_FIELDS))){if((options&4)!=0)System.arraycopy(node.live,0,framesBefore,i*PRESENTATION_FIELDS,PRESENTATION_FIELDS);interrupted.add(node.geometry.animation);}}
        for(Animation old:interrupted)old.animator.cancel();Animation animation=new Animation(completion,options,damping,velocity*duration/1000f);
        for(int i=0;i<changed.length;i++)if(before[i]!=after[i]){AlphaChannel channel=new AlphaChannel(changed[i],before[i],after[i],animation);alphaChannels.put(channel.token,channel);animation.channels.add(channel);}
        for(int i=0;i<tokens.length;i++){Node node=nodes.get(tokens[i]);if(node==null){View view=callback.view(tokens[i]);node=node(tokens[i]);node.view=view;}
            float[] from=Arrays.copyOfRange(framesBefore,i*PRESENTATION_FIELDS,i*PRESENTATION_FIELDS+PRESENTATION_FIELDS),to=Arrays.copyOfRange(framesAfter,i*PRESENTATION_FIELDS,i*PRESENTATION_FIELDS+PRESENTATION_FIELDS);node.model=to;node.opacity=new Opacity(Arrays.copyOfRange(ancestors,offsets[i],offsets[i+1]),Arrays.copyOfRange(model,offsets[i],offsets[i+1]));animation.affected.add(node);
            if(geometryChanged(from,to)){Geometry geometry=new Geometry(node,animation,from,to);node.geometry=geometry;animation.geometry.add(geometry);}
        }
        animations.add(animation);long elapsed=Math.max(0,android.os.SystemClock.uptimeMillis()-requested),playTime=Math.max(0,elapsed-delay);boolean infinite=(options&8)!=0;
        if(!infinite && elapsed>=delay && (duration==0 || playTime>=duration*(1+repeat))){animation.start();animation.finish(true);return;}
        if(repeat>Integer.MAX_VALUE-1 || duration*(1.0+repeat)>Long.MAX_VALUE){animation.finish(false);callback.failure("The requested animation duration is too large.");return;}
        animation.timeline=!infinite && repeat>0;animation.cycles=1+repeat;if(animation.timeline)animation.animator.setFloatValues(0,animation.cycles);animation.animator.setDuration(animation.timeline ? (long)(duration*animation.cycles) : duration);animation.animator.setStartDelay(Math.max(0,delay-elapsed));animation.animator.setRepeatCount(infinite && duration>0 ? ValueAnimator.INFINITE : 0);animation.animator.setRepeatMode((options&16)!=0 ? ValueAnimator.REVERSE : ValueAnimator.RESTART);
        animation.render();animation.animator.start();if(playTime>0)animation.animator.setCurrentPlayTime(playTime);
    }
    private static final class Layer {
        final float[] metrics,locations;final int[] options,colors;final Bitmap image;
        Layer(float[] metrics,int[] options,int[] colors,float[] locations,Bitmap image){this.metrics=metrics;this.options=options;this.colors=colors;this.locations=locations;this.image=image;}
    }
    private static final class Decoration extends Drawable {
        final boolean foreground;final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);final RectF rectangle=new RectF(),destination=new RectF();final Rect source=new Rect();Layer layer;Drawable fallback;Shader shader;int shaderWidth=-1,shaderHeight=-1;
        Decoration(boolean foreground){this.foreground=foreground;}
        @Override protected void onBoundsChange(Rect bounds){if(fallback!=null)fallback.setBounds(bounds);}
        @Override public void draw(Canvas canvas){if(layer==null)return;Rect bounds=getBounds();float width=bounds.width(),height=bounds.height(),radius=layer.metrics[1];if(width<=0 || height<=0)return;rectangle.set(bounds);paint.setShader(null);paint.setStyle(Paint.Style.FILL);
            if(foreground){if(fallback!=null)fallback.draw(canvas);float line=Math.min(layer.metrics[0],Math.min(width,height));if(line>0){float half=line/2;rectangle.inset(half,half);paint.setColor(layer.options[1]);paint.setStrokeWidth(line);paint.setStyle(Paint.Style.STROKE);canvas.drawRoundRect(rectangle,Math.max(0,radius-half),Math.max(0,radius-half),paint);}return;}
            if(layer.options[3]==0 && fallback!=null)fallback.draw(canvas);else {paint.setColor(layer.options[0]);canvas.drawRoundRect(rectangle,radius,radius,paint);}
            if(layer.colors!=null && layer.colors.length>0){if(layer.colors.length==1 || layer.metrics[2]==layer.metrics[4] && layer.metrics[3]==layer.metrics[5])paint.setColor(layer.colors[layer.colors.length-1]);else {if(shaderWidth!=bounds.width() || shaderHeight!=bounds.height()){shader=new LinearGradient(bounds.left+width*layer.metrics[2],bounds.top+height*layer.metrics[3],bounds.left+width*layer.metrics[4],bounds.top+height*layer.metrics[5],layer.colors,layer.locations!=null && layer.locations.length==layer.colors.length ? layer.locations : null,Shader.TileMode.CLAMP);shaderWidth=bounds.width();shaderHeight=bounds.height();}paint.setShader(shader);paint.setColor(0xffffffff);}canvas.drawRoundRect(rectangle,radius,radius,paint);paint.setShader(null);}
            if(layer.image!=null){Bitmap image=layer.image;float[] m=layer.metrics;int left=Math.max(0,Math.min(image.getWidth(),Math.round(m[6]*image.getWidth()))),top=Math.max(0,Math.min(image.getHeight(),Math.round(m[7]*image.getHeight()))),right=Math.max(left,Math.min(image.getWidth(),Math.round((m[6]+m[8])*image.getWidth()))),bottom=Math.max(top,Math.min(image.getHeight(),Math.round((m[7]+m[9])*image.getHeight())));source.set(left,top,right,bottom);if(right==left || bottom==top)return;
                float naturalWidth=source.width()*m[10],naturalHeight=source.height()*m[10],w=width,h=height;int gravity=layer.options[2];if(gravity==1 || gravity==2){float scale=gravity==1 ? Math.min(width/naturalWidth,height/naturalHeight) : Math.max(width/naturalWidth,height/naturalHeight);w=naturalWidth*scale;h=naturalHeight*scale;}else if(gravity>=3){w=naturalWidth;h=naturalHeight;}
                float x=gravity==6 || gravity==8 || gravity==10 ? 0 : gravity==7 || gravity==9 || gravity==11 ? width-w : (width-w)/2,y=gravity==4 || gravity==8 || gravity==9 ? 0 : gravity==5 || gravity==10 || gravity==11 ? height-h : (height-h)/2;destination.set(bounds.left+x,bounds.top+y,bounds.left+x+w,bounds.top+y+h);paint.setColor(0xffffffff);canvas.drawBitmap(image,source,destination,paint);}
        }
        @Override public void setAlpha(int alpha){paint.setAlpha(alpha);}
        @Override public void setColorFilter(ColorFilter filter){paint.setColorFilter(filter);}
        @Override public int getOpacity(){return PixelFormat.TRANSLUCENT;}
    }
}
