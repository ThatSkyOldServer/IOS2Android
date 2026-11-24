package local.ios.host;

import android.content.Context;
import android.graphics.*;
import android.text.*;
import android.text.style.*;
import java.util.HashMap;

final class TextRenderer {
    private final Context context;
    private final HashMap<String, Typeface> fonts = new HashMap<>();
    TextRenderer(Context context) { this.context=context; }
    static void link(SpannableStringBuilder text,int start,int end,String url) {
        if (url==null || url.isEmpty() || start>=end) return;
        text.setSpan(new URLSpan(url) {
            @Override public void onClick(android.view.View view) {
                try { super.onClick(view); }
                catch (android.content.ActivityNotFoundException | SecurityException unavailable) {
                    android.widget.Toast.makeText(view.getContext(),"Unable to open this link.",android.widget.Toast.LENGTH_SHORT).show();
                }
            }
        },start,end,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }
    private Typeface font(String name, int traits) {
        String key=name+":"+traits;
        Typeface result=fonts.get(key);
        if (result!=null) return result;
        boolean bold=(traits&2)!=0 || name.contains("Bold"), italic=(traits&1)!=0 || name.contains("Italic");
        String file=BuildProfile.fontFile(name,bold,italic);
        if (file!=null) {
            try { result=Typeface.createFromAsset(context.getAssets(),BuildProfile.BUNDLE_NAME+"/"+file); }
            catch (RuntimeException missing) { result=null; }
        }
        if (result==null) result=Typeface.create("sans-serif",(bold ? Typeface.BOLD : 0)|(italic ? Typeface.ITALIC : 0));
        fonts.put(key,result); return result;
    }
    float[] fontMetrics(String name, int traits, float size) {
        TextPaint paint=new TextPaint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(font(name,traits)); paint.setTextSize(size);
        Paint.FontMetrics metrics=paint.getFontMetrics();
        Rect capital=new Rect(),lowercase=new Rect();
        paint.getTextBounds("H",0,1,capital); paint.getTextBounds("x",0,1,lowercase);

        return new float[]{metrics.descent-metrics.ascent+metrics.leading,
                -metrics.ascent,-metrics.descent,metrics.leading,
                Math.max(0,-capital.top),Math.max(0,-lowercase.top)};
    }
    private static final class FontRun extends MetricAffectingSpan {
        final Typeface typeface; final float size,kern,baseline;
        FontRun(Typeface typeface,float size,float kern,float baseline) { this.typeface=typeface; this.size=size; this.kern=kern; this.baseline=baseline; }
        private void apply(TextPaint p) { p.setTypeface(typeface); p.setTextSize(size); p.setLetterSpacing(kern/size); p.baselineShift=-Math.round(baseline); }
        @Override public void updateMeasureState(TextPaint p) { apply(p); }
        @Override public void updateDrawState(TextPaint p) { apply(p); }
    }
    float run(SpannableStringBuilder text,int start,int end,String name,int traits,float size,int color,int alignment,float spacing,float kern,float baseline) {
        Typeface typeface=font(name,traits);
        text.setSpan(new FontRun(typeface,size,kern,baseline),start,end,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        text.setSpan(new ForegroundColorSpan(color),start,end,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        if (start==0 || text.charAt(start-1)=='\n') {
            int paragraphEnd=end;
            while (paragraphEnd<text.length() && text.charAt(paragraphEnd-1)!='\n') ++paragraphEnd;
            Layout.Alignment align=alignment==1 ? Layout.Alignment.ALIGN_CENTER : alignment==2 ? Layout.Alignment.ALIGN_OPPOSITE : Layout.Alignment.ALIGN_NORMAL;
            text.setSpan(new AlignmentSpan.Standard(align),start,paragraphEnd,Spanned.SPAN_PARAGRAPH);
            if (spacing!=0) text.setSpan((LineHeightSpan)(s,a,b,spanStart,v,fm)->{ fm.descent+=Math.round(spacing); fm.bottom=Math.max(fm.bottom,fm.descent); },start,paragraphEnd,Spanned.SPAN_PARAGRAPH);
        }
        TextPaint paint=new TextPaint(Paint.ANTI_ALIAS_FLAG); paint.setTypeface(typeface); paint.setTextSize(size);
        Paint.FontMetricsInt metrics=paint.getFontMetricsInt();
        return metrics.descent-metrics.ascent+metrics.leading;
    }
    private static final class Attachment extends ReplacementSpan {
        final Bitmap bitmap; final RectF bounds; final boolean template;
        Attachment(Bitmap bitmap,float[] bounds,boolean template) { this.bitmap=bitmap; this.bounds=new RectF(bounds[0],bounds[1],bounds[0]+bounds[2],bounds[1]+bounds[3]); this.template=template; }
        @Override public int getSize(Paint paint,CharSequence text,int start,int end,Paint.FontMetricsInt fm) {
            if (fm!=null) { paint.getFontMetricsInt(fm); fm.ascent=Math.min(fm.ascent,(int)Math.floor(-bounds.bottom)); fm.descent=Math.max(fm.descent,(int)Math.ceil(-bounds.top)); fm.top=Math.min(fm.top,fm.ascent); fm.bottom=Math.max(fm.bottom,fm.descent); }
            return (int)Math.ceil(bounds.width());
        }
        @Override public void draw(Canvas canvas,CharSequence text,int start,int end,float x,int top,int y,int bottom,Paint paint) {
            Paint imagePaint=new Paint(paint); imagePaint.setFilterBitmap(true);
            if (template) imagePaint.setColorFilter(new PorterDuffColorFilter(paint.getColor(),PorterDuff.Mode.SRC_IN));
            canvas.drawBitmap(bitmap,null,new RectF(x+bounds.left,y-bounds.bottom,x+bounds.right,y-bounds.top),imagePaint);
        }
    }
    void attachment(SpannableStringBuilder text,int index,byte[] png,float[] bounds,boolean template) {
        Bitmap bitmap=BitmapFactory.decodeByteArray(png,0,png.length);
        if (bitmap==null) throw new IllegalArgumentException("Invalid original text attachment image");
        text.setSpan(new Attachment(bitmap,bounds,template),index,index+1,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }
    static float[] measure(CharSequence text,float size,float limit,int lines,int lineBreak) {
        TextPaint paint=new TextPaint(Paint.ANTI_ALIAS_FLAG); paint.setTextSize(size);
        int width=Float.isFinite(limit)&&limit>0 ? Math.max(1,(int)Math.ceil(limit)) : 32768;
        float desired=Layout.getDesiredWidth(text,paint);
        width=Math.min(width,Math.max(1,(int)Math.ceil(desired)));

        StaticLayout.Builder builder=StaticLayout.Builder.obtain(text,0,text.length(),paint,width).setIncludePad(false)
            .setUseLineSpacingFromFallbacks(true)
            .setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE).setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
            .setMaxLines(lines==0 ? Integer.MAX_VALUE : lines);
        if(lineBreak>=3) builder.setEllipsizedWidth(width).setEllipsize(lineBreak==3 ? TextUtils.TruncateAt.START : lineBreak==5 ? TextUtils.TruncateAt.MIDDLE : TextUtils.TruncateAt.END);
        StaticLayout layout=builder.build();
        float actual=0; for (int i=0;i<layout.getLineCount();i++) actual=Math.max(actual,layout.getLineWidth(i));
        return new float[]{(float)Math.ceil(actual),layout.getHeight()};
    }
    static float[] measureAttributed(CharSequence text,float size,float limit,float height,int options) {
        TextPaint paint=new TextPaint(Paint.ANTI_ALIAS_FLAG);paint.setTextSize(size);float desired=Layout.getDesiredWidth(text,paint);int width=Float.isFinite(limit) ? Math.max(1,Math.min(1048576,(int)Math.ceil(limit))) : Math.max(1,Math.min(1048576,(int)Math.ceil(desired)));
        if((options&1)==0)width=Math.max(1,Math.min(1048576,(int)Math.ceil(desired)));else width=Math.min(width,Math.max(1,(int)Math.ceil(desired)));
        StaticLayout.Builder builder=StaticLayout.Builder.obtain(text,0,text.length(),paint,width).setIncludePad(false).setUseLineSpacingFromFallbacks(true).setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE).setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).setMaxLines((options&1)!=0 ? Integer.MAX_VALUE : 1);
        StaticLayout layout=builder.build();if((options&32)!=0 && Float.isFinite(height) && layout.getHeight()>height){int lines=Math.max(1,layout.getLineForVertical(Math.max(0,(int)height)));while(lines>1 && layout.getLineBottom(lines-1)>height)--lines;layout=builder.setMaxLines(lines).setEllipsizedWidth(width).setEllipsize(TextUtils.TruncateAt.END).build();}
        float actual=0;for(int i=0;i<layout.getLineCount();i++)actual=Math.max(actual,layout.getLineWidth(i));float measuredHeight=layout.getHeight();if((options&2)!=0){Paint.FontMetrics metrics=paint.getFontMetrics();measuredHeight+=Math.max(0,metrics.leading)*layout.getLineCount();}return new float[]{(float)Math.ceil(actual),measuredHeight};
    }
    static float[] singleLineMetrics(CharSequence text,float size) {
        TextPaint paint=new TextPaint(Paint.ANTI_ALIAS_FLAG);paint.setTextSize(size);float width=Layout.getDesiredWidth(text,paint);StaticLayout layout=StaticLayout.Builder.obtain(text,0,text.length(),paint,Math.max(1,Math.min(1048576,(int)Math.ceil(width)))).setIncludePad(false).setUseLineSpacingFromFallbacks(true).setMaxLines(1).build();return new float[]{width,layout.getHeight(),layout.getLineBaseline(0)};
    }
    static CharSequence scaled(CharSequence text,float scale) {
        if(scale==1 || !(text instanceof Spanned))return text;SpannableStringBuilder result=new SpannableStringBuilder(text);for(FontRun run:result.getSpans(0,result.length(),FontRun.class)){int start=result.getSpanStart(run),end=result.getSpanEnd(run),flags=result.getSpanFlags(run);result.removeSpan(run);result.setSpan(new FontRun(run.typeface,run.size*scale,run.kern*scale,run.baseline*scale),start,end,flags);}return result;
    }
    byte[] fontBytes(String name) {
        String file=BuildProfile.fontFile(name,name.contains("Bold"),name.contains("Italic"));
        if (file==null) return null;
        try (java.io.InputStream input=context.getAssets().open(BuildProfile.BUNDLE_NAME+"/"+file); java.io.ByteArrayOutputStream output=new java.io.ByteArrayOutputStream()) {
            byte[] buffer=new byte[16384]; int count;
            while ((count=input.read(buffer))!=-1) output.write(buffer,0,count);
            return output.toByteArray();
        } catch (java.io.IOException missing) { return null; }
    }
    static byte[] convertImage(byte[] bytes,boolean jpeg,int quality) {
        Bitmap bitmap=BitmapFactory.decodeByteArray(bytes,0,bytes.length);
        if (bitmap==null) return null;
        try { return encodeImage(bitmap,jpeg,quality); } finally { bitmap.recycle(); }
    }
    private static byte[] encodeImage(Bitmap bitmap,boolean jpeg,int quality) {
        java.io.ByteArrayOutputStream output=new java.io.ByteArrayOutputStream();
        return bitmap.compress(jpeg ? Bitmap.CompressFormat.JPEG : Bitmap.CompressFormat.PNG,quality,output) ? output.toByteArray() : null;
    }
    private static final class DrawingState {
        Paint fill=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG),stroke=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
        float alpha=1;
        DrawingState() { stroke.setStyle(Paint.Style.STROKE); stroke.setStrokeWidth(1); }
        DrawingState(DrawingState source) { fill=new Paint(source.fill); stroke=new Paint(source.stroke); alpha=source.alpha; }
        Paint paint(boolean outline) { Paint result=new Paint(outline ? stroke : fill); result.setAlpha(Math.round(result.getAlpha()*alpha)); return result; }
    }
    private static final class DrawingMask {
        final Bitmap image; final Matrix matrix; final int save;
        DrawingMask(Bitmap image,Matrix matrix,int save) { this.image=image; this.matrix=matrix; this.save=save; }
    }
    private static final class DrawingFrame {
        final int save; final DrawingState state; final boolean layer;
        final java.util.ArrayList<DrawingMask> masks=new java.util.ArrayList<>();
        DrawingFrame(int save,DrawingState state,boolean layer) { this.save=save; this.state=state; this.layer=layer; }
    }
    private static final class Drawing {
        final Canvas canvas; final Bitmap[] images;
        final Path path=new Path(); final Matrix matrix=new Matrix(),inverse=new Matrix(); final float[] point=new float[6];
        final java.util.ArrayList<DrawingFrame> frames=new java.util.ArrayList<>();
        DrawingState state=new DrawingState(); float currentX,currentY,startX,startY; boolean hasPoint;
        Drawing(Canvas canvas,Bitmap[] images) { this.canvas=canvas; this.images=images; frames.add(new DrawingFrame(canvas.getSaveCount(),new DrawingState(state),false)); }
        void point(int op,float... xy) {
            canvas.getMatrix(matrix); System.arraycopy(xy,0,point,0,xy.length); matrix.mapPoints(point,0,point,0,xy.length/2);
            if (!hasPoint && op!=12) { path.moveTo(point[0],point[1]); startX=point[0]; startY=point[1]; hasPoint=true; }
            if (op==12) { path.moveTo(point[0],point[1]); startX=point[0]; startY=point[1]; }
            else if (op==13) path.lineTo(point[0],point[1]);
            else if (op==14) path.cubicTo(point[0],point[1],point[2],point[3],point[4],point[5]);
            else path.quadTo(point[0],point[1],point[2],point[3]);
            currentX=point[xy.length-2]; currentY=point[xy.length-1]; hasPoint=true;
        }
        void arc(double x,double y,double r,double start,double end,boolean clockwise) {
            if (r<0 || !Double.isFinite(r) || !Double.isFinite(start) || !Double.isFinite(end)) throw new IllegalArgumentException("Invalid arc");
            double delta=end-start;
            if (Math.abs(delta)>=2*Math.PI) delta=clockwise ? -2*Math.PI : 2*Math.PI;
            else if (clockwise && delta>0) delta-=2*Math.PI; else if (!clockwise && delta<0) delta+=2*Math.PI;
            point(hasPoint ? 13 : 12,(float)(x+r*Math.cos(start)),(float)(y+r*Math.sin(start)));
            int count=(int)Math.ceil(Math.abs(delta)/(Math.PI/2));
            for (int i=0;i<count;i++) {
                double a=start+delta*i/count,b=start+delta*(i+1)/count,k=4.0/3*Math.tan((b-a)/4);
                point(14,(float)(x+r*(Math.cos(a)-k*Math.sin(a))),(float)(y+r*(Math.sin(a)+k*Math.cos(a))),
                    (float)(x+r*(Math.cos(b)+k*Math.sin(b))),(float)(y+r*(Math.sin(b)-k*Math.cos(b))),(float)(x+r*Math.cos(b)),(float)(y+r*Math.sin(b)));
            }
        }
        void arcTo(double x1,double y1,double x2,double y2,double radius) {
            if (radius<0) throw new IllegalArgumentException("Invalid arc radius");
            if (!hasPoint || radius==0) { point(hasPoint ? 13 : 12,(float)x1,(float)y1); return; }
            canvas.getMatrix(matrix);
            if (!matrix.invert(inverse)) { point(13,(float)x1,(float)y1); return; }
            point[0]=currentX; point[1]=currentY; inverse.mapPoints(point,0,point,0,1);
            double ux=point[0]-x1,uy=point[1]-y1,vx=x2-x1,vy=y2-y1,ul=Math.hypot(ux,uy),vl=Math.hypot(vx,vy);
            if (ul==0 || vl==0) { point(13,(float)x1,(float)y1); return; }
            ux/=ul; uy/=ul; vx/=vl; vy/=vl; double cross=ux*vy-uy*vx;
            if (Math.abs(cross)<1e-12) { point(13,(float)x1,(float)y1); return; }
            double length=radius/Math.tan(Math.acos(Math.max(-1,Math.min(1,ux*vx+uy*vy)))/2),side=cross>0 ? 1 : -1;
            double ax=x1+ux*length,ay=y1+uy*length,cx=ax-uy*side*radius,cy=ay+ux*side*radius,bx=x1+vx*length,by=y1+vy*length;
            point(13,(float)ax,(float)ay); arc(cx,cy,radius,Math.atan2(ay-cy,ax-cx),Math.atan2(by-cy,bx-cx),cross>0);
        }
        Path localPath() { canvas.getMatrix(matrix); Path local=new Path(); if (matrix.invert(inverse)) path.transform(inverse,local); return local; }
        void resetPath() { path.reset(); hasPoint=false; }
        void drawPath(int mode) {
            Path local=localPath(); local.setFillType(mode==1 || mode==4 ? Path.FillType.EVEN_ODD : Path.FillType.WINDING);
            if (mode!=2) canvas.drawPath(local,state.paint(false));
            if (mode>=2) canvas.drawPath(local,state.paint(true));
            resetPath();
        }
        void save(boolean layer) {
            DrawingState previous=new DrawingState(state);
            Paint composite=state.paint(false); composite.setColor(Color.WHITE); composite.setAlpha(Math.round(state.alpha*255));
            int saved=layer ? canvas.saveLayer(null,composite) : canvas.save();
            frames.add(new DrawingFrame(saved,previous,layer));
            if (layer) { state.alpha=1; state.fill.setXfermode(null); state.stroke.setXfermode(null); state.fill.clearShadowLayer(); state.stroke.clearShadowLayer(); }
        }
        void applyMasks(DrawingFrame frame) {
            Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG); paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_IN));
            for (int i=frame.masks.size()-1;i>=0;i--) {
                DrawingMask mask=frame.masks.get(i); canvas.save(); canvas.setMatrix(new Matrix()); canvas.drawBitmap(mask.image,mask.matrix,paint); canvas.restore(); canvas.restoreToCount(mask.save);
            }
        }
        void restore(boolean layer) {
            if (frames.size()==1 || frames.get(frames.size()-1).layer!=layer) throw new IllegalArgumentException("Unbalanced drawing state");
            DrawingFrame frame=frames.remove(frames.size()-1); applyMasks(frame); canvas.restoreToCount(frame.save); state=frame.state;
        }
        static int color(double[] v,int i) { return Color.argb(channel(v[i+3]),channel(v[i]),channel(v[i+1]),channel(v[i+2])); }
        static int channel(double value) { return (int)Math.round(Math.max(0,Math.min(1,value))*255); }
        static RectF rect(double[] v,int i) { float x=(float)v[i],y=(float)v[i+1],w=(float)v[i+2],h=(float)v[i+3]; return new RectF(Math.min(x,x+w),Math.min(y,y+h),Math.max(x,x+w),Math.max(y,y+h)); }
        static PorterDuff.Mode blend(int mode) {
            switch(mode) {
                case 0:return PorterDuff.Mode.SRC_OVER; case 1:return PorterDuff.Mode.MULTIPLY; case 2:return PorterDuff.Mode.SCREEN;
                case 16:return PorterDuff.Mode.CLEAR; case 17:return PorterDuff.Mode.SRC; case 18:return PorterDuff.Mode.SRC_IN;
                case 19:return PorterDuff.Mode.SRC_OUT; case 20:return PorterDuff.Mode.SRC_ATOP; case 21:return PorterDuff.Mode.DST_OVER;
                case 22:return PorterDuff.Mode.DST_IN; case 23:return PorterDuff.Mode.DST_OUT; case 24:return PorterDuff.Mode.DST_ATOP;
                case 25:return PorterDuff.Mode.XOR; case 27:return PorterDuff.Mode.ADD;
                default:throw new IllegalArgumentException("Unsupported drawing blend mode: "+mode);
            }
        }
        void image(double[] v,int i,boolean mask) {
            Bitmap bitmap=images[(int)v[i+4]]; if (bitmap==null) throw new IllegalArgumentException("Invalid drawing image");
            RectF rect=rect(v,i); boolean flip=v[i+5]!=0;
            Matrix map=new Matrix(); map.setValues(new float[]{rect.width()/bitmap.getWidth(),0,rect.left,0,(flip ? -1 : 1)*rect.height()/bitmap.getHeight(),flip ? rect.bottom : rect.top,0,0,1});
            if (mask) {
                int save=canvas.saveLayer(rect,null); canvas.clipRect(rect); canvas.getMatrix(matrix); map.postConcat(matrix);
                frames.get(frames.size()-1).masks.add(new DrawingMask(bitmap,map,save));
            } else canvas.drawBitmap(bitmap,map,state.paint(false));
        }
        void gradient(double[] v,int i) {
            boolean radial=v[i]!=0; float x0=(float)v[i+1],y0=(float)v[i+2],r0=(float)v[i+3],x1=(float)v[i+4],y1=(float)v[i+5],r1=(float)v[i+6];
            int options=(int)v[i+7],count=(int)v[i+8]; int[] colors=new int[Math.max(2,count)]; float[] stops=new float[colors.length];
            for(int n=0;n<count;n++) { stops[n]=(float)v[i+9+n*5]; colors[n]=color(v,i+10+n*5); }
            if(count==1) { colors[1]=colors[0]; stops[0]=0; stops[1]=1; }
            Shader shader;
            if (radial) {
                if (x0==x1 && y0==y1 && r0==0) shader=new RadialGradient(x1,y1,r1,colors,stops,Shader.TileMode.CLAMP);
                else if (android.os.Build.VERSION.SDK_INT>=31) {
                    long[] packed=new long[colors.length]; for(int n=0;n<colors.length;n++) packed[n]=Color.pack(colors[n]);
                    shader=new RadialGradient(x0,y0,r0,x1,y1,r1,packed,stops,Shader.TileMode.CLAMP);
                } else throw new IllegalArgumentException("Two-circle gradients require Android 12");
            } else shader=new LinearGradient(x0,y0,x1,y1,colors,stops,Shader.TileMode.CLAMP);
            int saved=canvas.save();
            if (radial) {
                if ((options&2)==0) { Path circle=new Path(); circle.addCircle(x1,y1,r1,Path.Direction.CW); canvas.clipPath(circle); }
                if ((options&1)==0 && r0>0) { Path circle=new Path(); circle.addCircle(x0,y0,r0,Path.Direction.CW); canvas.clipOutPath(circle); }
            } else if ((options&3)!=3) {
                double dx=x1-x0,dy=y1-y0,length=Math.hypot(dx,dy);
                if (length>0) {
                    float nx=(float)(-dy/length*1e6),ny=(float)(dx/length*1e6),tx=(float)(dx/length*1e6),ty=(float)(dy/length*1e6);
                    float ax=(options&1)!=0 ? x0-tx : x0,ay=(options&1)!=0 ? y0-ty : y0,bx=(options&2)!=0 ? x1+tx : x1,by=(options&2)!=0 ? y1+ty : y1;
                    Path band=new Path(); band.moveTo(ax+nx,ay+ny); band.lineTo(bx+nx,by+ny); band.lineTo(bx-nx,by-ny); band.lineTo(ax-nx,ay-ny); band.close(); canvas.clipPath(band);
                }
            }
            Paint paint=state.paint(false); paint.setColor(Color.WHITE); paint.setAlpha(Math.round(state.alpha*255)); paint.setShader(shader);
            canvas.drawPaint(paint); canvas.restoreToCount(saved);
        }
        void execute(double[] v,int start,int end) {
            for (int offset=start;offset<end;) {
                int op=(int)v[offset],count=(int)v[offset+1],i=offset+2;
                if (count<0 || count>end-i) throw new IllegalArgumentException("Invalid drawing command");
                switch(op) {
                    case 1:save(false);break; case 2:restore(false);break;
                    case 3:matrix.setValues(new float[]{(float)v[i],(float)v[i+2],(float)v[i+4],(float)v[i+1],(float)v[i+3],(float)v[i+5],0,0,1});canvas.concat(matrix);break;
                    case 4:state.fill.setColor(color(v,i));break; case 5:state.stroke.setColor(color(v,i));break;
                    case 6:state.stroke.setStrokeWidth((float)v[i]);break;
                    case 7:state.stroke.setStrokeJoin(v[i]==0 ? Paint.Join.MITER : v[i]==1 ? Paint.Join.ROUND : Paint.Join.BEVEL);break;
                    case 8:state.stroke.setStrokeMiter((float)v[i]);break; case 9:state.alpha=(float)Math.max(0,Math.min(1,v[i]));break;
                    case 10:PorterDuffXfermode mode=new PorterDuffXfermode(blend((int)v[i]));state.fill.setXfermode(mode);state.stroke.setXfermode(mode);break;
                    case 11:int shadow=color(v,i+3);state.fill.setShadowLayer((float)v[i+2],(float)v[i],(float)v[i+1],shadow);state.stroke.setShadowLayer((float)v[i+2],(float)v[i],(float)v[i+1],shadow);break;
                    case 12:case 13:point(op,(float)v[i],(float)v[i+1]);break;
                    case 14:point(op,(float)v[i],(float)v[i+1],(float)v[i+2],(float)v[i+3],(float)v[i+4],(float)v[i+5]);break;
                    case 15:point(op,(float)v[i],(float)v[i+1],(float)v[i+2],(float)v[i+3]);break;
                    case 16:if(hasPoint){path.close();currentX=startX;currentY=startY;}break;
                    case 17:resetPath();break; case 18:arc(v[i],v[i+1],v[i+2],v[i+3],v[i+4],v[i+5]!=0);break;
                    case 19:arcTo(v[i],v[i+1],v[i+2],v[i+3],v[i+4]);break; case 20:execute(v,i,i+count);break;
                    case 21:drawPath((int)v[i]);break;
                    case 22:if(v[i]==2)canvas.clipRect(rect(v,i+1));else{Path local=localPath();local.setFillType(v[i]==0 ? Path.FillType.WINDING : Path.FillType.EVEN_ODD);canvas.clipPath(local);resetPath();}break;
                    case 23:case 24:Paint paint=state.paint(v[i]==1);if(v[i]==2)paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.CLEAR));if(op==23)canvas.drawRect(rect(v,i+1),paint);else canvas.drawOval(rect(v,i+1),paint);break;
                    case 25:gradient(v,i);break; case 26:image(v,i,true);break; case 27:image(v,i,false);break;
                    case 28:save(true);break; case 29:restore(true);break;
                    case 30:point(12,(float)v[i],(float)v[i+1]);point(13,(float)(v[i]+v[i+2]),(float)v[i+1]);point(13,(float)(v[i]+v[i+2]),(float)(v[i+1]+v[i+3]));point(13,(float)v[i],(float)(v[i+1]+v[i+3]));path.close();currentX=startX;currentY=startY;break;
                    case 31:if(count==1)state.stroke.setPathEffect(null);else{int n=count-1;float[] dash=new float[n%2==0 ? n : n*2];for(int k=0;k<dash.length;k++)dash[k]=(float)v[i+1+k%n];state.stroke.setPathEffect(new DashPathEffect(dash,(float)v[i]));}break;
                    default:throw new IllegalArgumentException("Unknown drawing command: "+op);
                }
                offset=i+count;
            }
        }
        void finish() { while(frames.size()>1)restore(frames.get(frames.size()-1).layer);applyMasks(frames.get(0)); }
    }
    static byte[] renderImage(int width,int height,float scale,boolean opaque,double[] commands,byte[][] sources) {
        Bitmap bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888); Bitmap[] images=new Bitmap[sources.length];
        try {
            if (opaque) { bitmap.eraseColor(Color.BLACK); bitmap.setHasAlpha(false); }
            for(int i=0;i<sources.length;i++) { images[i]=BitmapFactory.decodeByteArray(sources[i],0,sources[i].length);if(images[i]==null)throw new IllegalArgumentException("Invalid drawing source image"); }
            Canvas canvas=new Canvas(bitmap);canvas.scale(scale,scale);Drawing drawing=new Drawing(canvas,images);drawing.execute(commands,0,commands.length);drawing.finish();
            return encodeImage(bitmap,false,100);
        } finally { for(Bitmap image:images)if(image!=null)image.recycle();bitmap.recycle(); }
    }
}
