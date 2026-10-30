/* PES6PORT — Wi-Fi Direct for WIRELESS mode (docs/15 §11).
 *
 * Two phones join one Wi-Fi Direct group with fixed credentials, so their
 * datagrams go straight from one radio to the other instead of through the
 * router (or with no router at all). net.c does not change: the group is an
 * ordinary IPv4 subnet (192.168.49.x), its HELLOs find the other side there.
 *
 * Android 10+ lets an app create a group with its own name and passphrase and
 * join one by them without any dialog, which is all this uses. Roles:
 *   host  create the group (owner, 192.168.49.1)
 *   join  connect to it, retrying until it exists
 *   auto  alternate: try to join for a while, then host for a while, with
 *         random lengths so that two phones started together end up apart
 *
 * Called from the native host (runtime/android/main.c) through JNI; every
 * callback runs on a thread of its own here, and the native side only polls
 * state() and ownerAddress().
 */
package com.pes6port;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.wifi.p2p.WifiP2pConfig;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import java.util.Random;

public final class WifiDirect {
    private static final String TAG = "pes6";
    private static final int TICK_MS = 1000;

    private enum Phase { IDLE, JOINING, HOSTING }

    private final Activity activity;
    private final String mode;
    private final WifiP2pConfig config;
    private final HandlerThread thread = new HandlerThread("pes6-wifi-direct");
    private final Handler handler;
    private final Random random = new Random();
    private WifiP2pManager manager;
    private WifiP2pManager.Channel channel;
    private Phase phase = Phase.IDLE;
    private long phaseEnd;                       /* elapsedRealtime: when to switch (auto) or retry */
    private boolean asked, stopped;
    private volatile String state = "starting";
    private volatile String owner;

    /* Returns null when the device cannot do it (before Android 10, no Wi-Fi Direct). */
    public static WifiDirect start(Activity activity, String mode, String name, String passphrase) {
        if (Build.VERSION.SDK_INT < 29) {
            Log.w(TAG, "Wi-Fi Direct: needs Android 10");
            return null;
        }
        if (!activity.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT)) {
            Log.w(TAG, "Wi-Fi Direct: not on this device");
            return null;
        }
        return new WifiDirect(activity, mode, name, passphrase);
    }

    private WifiDirect(Activity activity, String mode, String name, String passphrase) {
        this.activity = activity;
        this.mode = mode;
        this.config = new WifiP2pConfig.Builder()
                .setNetworkName(name)
                .setPassphrase(passphrase)
                .setGroupOperatingBand(WifiP2pConfig.GROUP_OWNER_BAND_AUTO)  /* the router's channel when there is one */
                .enablePersistentMode(false)
                .build();
        thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(this::tick);
    }

    /* "host, 1 player(s)", "joined", "joining", ... -- for the log. */
    public String state() { return state; }

    /* The group owner's address once in a group (the other side, when we joined), else null. */
    public String ownerAddress() { return owner; }

    public void stop() {
        handler.post(() -> {
            stopped = true;
            owner = null;
            state = "stopped";
            if (manager != null) {
                manager.cancelConnect(channel, null);
                manager.removeGroup(channel, null);
            }
            handler.postDelayed(thread::quitSafely, 2000);
        });
    }

    private String permission() {
        return Build.VERSION.SDK_INT >= 33 ? Manifest.permission.NEARBY_WIFI_DEVICES
                                           : Manifest.permission.ACCESS_FINE_LOCATION;
    }

    private void tick() {
        if (stopped) return;
        final String perm = permission();
        if (activity.checkSelfPermission(perm) != PackageManager.PERMISSION_GRANTED) {
            if (!asked) {
                asked = true;
                activity.runOnUiThread(() -> activity.requestPermissions(new String[] { perm }, 0x7e6));
            }
            state = "waiting for permission " + perm;
            handler.postDelayed(this::tick, TICK_MS);
            return;
        }
        if (manager == null) {
            manager = (WifiP2pManager) activity.getApplicationContext().getSystemService(Context.WIFI_P2P_SERVICE);
            if (manager == null) { state = "no Wi-Fi Direct service"; return; }
            channel = manager.initialize(activity.getApplicationContext(), thread.getLooper(),
                    () -> { state = "channel lost"; manager = null; });
        }
        manager.requestConnectionInfo(channel, this::onInfo);
    }

    private void onInfo(WifiP2pInfo info) {
        if (stopped) return;
        final long now = SystemClock.elapsedRealtime();
        if (info != null && info.groupFormed) {
            owner = info.groupOwnerAddress != null ? info.groupOwnerAddress.getHostAddress() : null;
            if (!info.isGroupOwner) {
                state = "joined (owner " + owner + ")";
                phase = Phase.JOINING;
                handler.postDelayed(this::tick, TICK_MS);
                return;
            }
            phase = Phase.HOSTING;
            manager.requestGroupInfo(channel, group -> onHosting(group, now));
            return;
        }
        owner = null;
        if (phase == Phase.IDLE) {
            if (mode.equals("host")) host(now); else join(now);
        } else if (now >= phaseEnd) {
            /* Nothing yet: in auto, change role; otherwise try again. */
            if (phase == Phase.JOINING) {
                manager.cancelConnect(channel, null);
                if (mode.equals("auto")) host(now); else join(now);
            } else {
                if (mode.equals("auto")) join(now); else host(now);
            }
        }
        handler.postDelayed(this::tick, TICK_MS);
    }

    private void onHosting(WifiP2pGroup group, long now) {
        if (stopped) return;
        final int players = group != null ? group.getClientList().size() : 0;
        state = "host, " + players + " player(s) joined";
        if (players == 0 && mode.equals("auto") && now >= phaseEnd) {
            /* Alone for a while: maybe the other phone is hosting too. Go and look. */
            Log.i(TAG, "Wi-Fi Direct: nobody joined, trying to join instead");
            manager.removeGroup(channel, null);
            owner = null;
            join(now);
        }
        handler.postDelayed(this::tick, TICK_MS);
    }

    private void host(long now) {
        phase = Phase.HOSTING;
        phaseEnd = now + 15000 + random.nextInt(25000);
        state = "creating the group";
        manager.removeGroup(channel, null);
        handler.postDelayed(() -> manager.createGroup(channel, config, listener("createGroup")), 500);
    }

    private void join(long now) {
        phase = Phase.JOINING;
        phaseEnd = now + 8000 + random.nextInt(6000);
        state = "joining";
        manager.connect(channel, config, listener("connect"));
    }

    private WifiP2pManager.ActionListener listener(final String what) {
        return new WifiP2pManager.ActionListener() {
            @Override public void onSuccess() { Log.i(TAG, "Wi-Fi Direct: " + what + " requested"); }
            @Override public void onFailure(int reason) {
                /* 0 ERROR, 1 P2P_UNSUPPORTED, 2 BUSY, 3 NO_SERVICE_REQUESTS */
                Log.w(TAG, "Wi-Fi Direct: " + what + " failed, reason " + reason);
                state = what + " failed (" + reason + ")";
            }
        };
    }
}
