package local.ios.host;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioPlaybackConfiguration;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Handler;
import android.util.Log;
import java.util.Collections;
import java.util.List;
import java.util.function.BooleanSupplier;

final class AudioSession {
    private static native String changed(int event, int value);
    private static native int ownedOutputs();
    private final AudioManager manager;
    private final AudioOutput sound;
    private final BooleanSupplier loaded;
    private final HttpTransport.FailureSink failures;
    private final AudioAttributes attributes = new AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
    private double preferredRate, preferredBuffer;
    private int preferredChannels;
    private boolean active, otherAudio, silenceSecondaryAudio;
    private final AudioDeviceCallback devices = new AudioDeviceCallback() {
        @Override public void onAudioDevicesAdded(AudioDeviceInfo[] added) { notifyGuest(0, 1); }
        @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) { notifyGuest(0, 2); }
    };
    private final AudioManager.AudioPlaybackCallback playback = new AudioManager.AudioPlaybackCallback() {
        @Override public void onPlaybackConfigChanged(List<AudioPlaybackConfiguration> configurations) {
            int state = externalPlayback(configurations);
            boolean notify, next = (state & 2) != 0;
            synchronized (AudioSession.this) {
                notify = active && next != silenceSecondaryAudio;
                otherAudio = (state & 1) != 0;
                silenceSecondaryAudio = next;
            }
            if (notify) {
                if (BuildProfile.DEVELOPMENT) Log.i("I2AGuest", "Audio session secondary audio hint: " + next);
                notifyGuest(1, next ? 1 : 0);
            }
        }
    };
    private int externalPlayback(List<AudioPlaybackConfiguration> configurations) {
        int owned=sound.outputCount()+(loaded.getAsBoolean() ? ownedOutputs() : 0);
        int state=0;
        for(AudioPlaybackConfiguration configuration:configurations) {
            AudioAttributes attributes=configuration.getAudioAttributes();
            int usage=attributes.getUsage(), content=attributes.getContentType();
            if(usage==AudioAttributes.USAGE_GAME && owned>0) { --owned; continue; }
            state|=1;
            // Public playback configurations omit client identity and audio focus.
            if(content!=AudioAttributes.CONTENT_TYPE_SONIFICATION &&
                (usage==AudioAttributes.USAGE_MEDIA || usage==AudioAttributes.USAGE_GAME ||
                 (usage==AudioAttributes.USAGE_UNKNOWN && (content==AudioAttributes.CONTENT_TYPE_MUSIC || content==AudioAttributes.CONTENT_TYPE_MOVIE)))) state|=2;
        }
        return state;
    }
    AudioSession(Context context, Handler main, AudioOutput sound, BooleanSupplier loaded, HttpTransport.FailureSink failures) {
        this.manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        this.sound = sound; this.loaded = loaded; this.failures = failures;
        int state = externalPlayback(manager.getActivePlaybackConfigurations());
        otherAudio = (state & 1) != 0; silenceSecondaryAudio = (state & 2) != 0;
        manager.registerAudioDeviceCallback(devices, main);
        manager.registerAudioPlaybackCallback(playback, main);
    }
    private void notifyGuest(int event, int value) {
        if (!loaded.getAsBoolean()) return;
        String failure = changed(event, value);
        if (failure != null) failures.report(failure);
    }
    void close() { manager.unregisterAudioDeviceCallback(devices); manager.unregisterAudioPlaybackCallback(playback); }
    private List<AudioDeviceInfo> outputDevices() {
        if (Build.VERSION.SDK_INT >= 33) return manager.getAudioDevicesForAttributes(attributes);
        AudioDeviceInfo routed = sound.routedDevice();
        return routed == null ? Collections.emptyList() : Collections.singletonList(routed);
    }
    private int outputRate() {
        String value = manager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
        if (value != null) try { int rate = Integer.parseInt(value); if (rate > 0) return rate; } catch (NumberFormatException ignored) {}
        return AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC);
    }
    private int outputChannels() {
        int maximum = 0;
        for (AudioDeviceInfo device : outputDevices()) for (int count : device.getChannelCounts()) maximum = Math.max(maximum, count);

        return maximum == 0 ? 2 : Math.min(maximum, 2);
    }
    synchronized int configure(int property, double value) {
        if (!Double.isFinite(value)) return -50;
        if (property == 0) {
            if (value != 0 && value != 1) return -50;
            if (value == 0 && sound.anyPlaying()) return -50;
            if (value != 0 && outputRate() <= 0) return -50;
            active = value != 0;
            if (active) {
                int state = externalPlayback(manager.getActivePlaybackConfigurations());
                otherAudio = (state & 1) != 0; silenceSecondaryAudio = (state & 2) != 0;
            }
            if (active) Log.i("I2AGuest", "Audio session: output=" + outputRate() + " Hz, " + outputChannels()
                + " channels; preferred=" + preferredRate + " Hz, " + preferredBuffer + " s; otherAudio=" + otherAudio
                + "; secondaryAudio=" + silenceSecondaryAudio);
        } else if (property == 1) {
            if (value < 8000 || value > 192000 || value != Math.rint(value)) return -50;
            if (AudioTrack.getMinBufferSize((int) value, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT) <= 0) return -50;
            preferredRate = value;
        } else if (property == 2) {
            if (value <= 0 || value > 1) return -50;
            preferredBuffer = value;
        } else if (property == 3) {
            if (value < 1 || value > outputChannels() || value != Math.rint(value)) return -50;
            preferredChannels = (int) value;
        } else return -50;
        return 0;
    }
    synchronized double property(int property) {
        if (property == 0) return active && preferredRate > 0 ? preferredRate : outputRate();
        if (property == 1) return outputChannels();
        if (property == 2) {
            String frames = manager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);
            if (frames != null) try { return Integer.parseInt(frames) / (double) outputRate(); } catch (NumberFormatException ignored) {}
             
            return 0;
        }
        if (property == 3) return otherAudio ? 1 : 0;
        if (property == 4) return preferredRate;
        if (property == 5) return preferredBuffer;
        if (property == 6) return preferredChannels;
        if (property == 7) {
            int maximum = manager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
            return maximum == 0 ? 0 : manager.getStreamVolume(AudioManager.STREAM_MUSIC) / (double) maximum;
        }
        if (property == 8) return silenceSecondaryAudio ? 1 : 0;
        return 0;
    }
    String[][] route() {
        List<AudioDeviceInfo> devices = outputDevices();
        String[][] result = new String[devices.size()][3];
        for (int i = 0; i < devices.size(); i++) {
            AudioDeviceInfo device = devices.get(i);
            result[i] = new String[] {portType(device.getType()), device.getProductName().toString(), Integer.toString(device.getId())};
        }
        return result;
    }
    private static String portType(int type) {
        switch (type) {
            case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER: case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE: return "Speaker";
            case AudioDeviceInfo.TYPE_BUILTIN_EARPIECE: return "Receiver";
            case AudioDeviceInfo.TYPE_WIRED_HEADPHONES: case AudioDeviceInfo.TYPE_WIRED_HEADSET: return "Headphones";
            case AudioDeviceInfo.TYPE_BLUETOOTH_A2DP: return "BluetoothA2DPOutput";
            case AudioDeviceInfo.TYPE_BLUETOOTH_SCO: return "BluetoothHFP";
            case AudioDeviceInfo.TYPE_BLE_HEADSET: case AudioDeviceInfo.TYPE_BLE_SPEAKER: return "BluetoothLE";
            case AudioDeviceInfo.TYPE_USB_DEVICE: case AudioDeviceInfo.TYPE_USB_ACCESSORY: case AudioDeviceInfo.TYPE_USB_HEADSET: return "USBAudio";
            case AudioDeviceInfo.TYPE_HDMI: case AudioDeviceInfo.TYPE_HDMI_ARC: case AudioDeviceInfo.TYPE_HDMI_EARC: return "HDMIOutput";
            case AudioDeviceInfo.TYPE_LINE_ANALOG: case AudioDeviceInfo.TYPE_LINE_DIGITAL: return "LineOut";
            default: return "AndroidAudioDevice:" + type;
        }
    }
}
