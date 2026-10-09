# T2AudioPort - WASAPI render probe.
#
# Opens the T2 endpoint via WASAPI (shared mode), prints the mix format and the
# HRESULT of Initialize/GetBufferSize/Start/Stop. Used to find out why the audio
# engine allocates the WaveRT buffer but never transitions the stream to RUN.
#
# All COM work is done inside C# so the RCW-to-interface casts happen in the
# CLR (PowerShell cannot cast a raw COM object to a [ComImport] interface).

param(
    [string]$DeviceId = '{0.0.0.00000000}.{aae9601c-668b-4949-b91d-c8a60566f8b5}'
)

$ErrorActionPreference = 'Stop'

$code = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace T2Wasapi {
public enum EDataFlow { eRender, eCapture, eAll }
public enum ERole { eConsole, eMultimedia, eCommunications }
[Flags] public enum CLSCTX : uint { INPROC_SERVER=1, INPROC_HANDLER=2, LOCAL_SERVER=4, REMOTE_SERVER=0x10, ALL=0x17 }

[StructLayout(LayoutKind.Sequential, Pack=2)]
public struct WAVEFORMATEX {
    public ushort wFormatTag; public ushort nChannels; public uint nSamplesPerSec;
    public uint nAvgBytesPerSec; public ushort nBlockAlign; public ushort wBitsPerSample; public ushort cbSize;
}
[StructLayout(LayoutKind.Sequential, Pack=2)]
public struct WAVEFORMATEXTENSIBLE {
    public WAVEFORMATEX Format; public ushort wValidBitsPerSample; public uint dwChannelMask; public Guid SubFormat;
}

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] public class MMDeviceEnumerator { }

[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(EDataFlow dataFlow, uint dwStateMask, out IMMDeviceCollection devices);
    [PreserveSig] int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);
    [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
    [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
    [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
}
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceCollection {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int Item(uint index, out IMMDevice device);
}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, CLSCTX clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    [PreserveSig] int OpenPropertyStore(uint access, out IntPtr props);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    [PreserveSig] int GetState(out uint state);
}
[Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioClient {
    [PreserveSig] int Initialize(int shareMode, uint streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr pFormat, IntPtr audioSessionGuid);
    [PreserveSig] int GetBufferSize(out uint numFrames);
    [PreserveSig] int GetStreamLatency(out long latency);
    [PreserveSig] int GetCurrentPadding(out uint padding);
    [PreserveSig] int IsFormatSupported(int shareMode, IntPtr pFormat, out IntPtr closestMatch);
    [PreserveSig] int GetMixFormat(out IntPtr ppDeviceFormat);
    [PreserveSig] int GetDevicePeriod(out long defaultPeriod, out long minPeriod);
    [PreserveSig] int Start();
    [PreserveSig] int Stop();
    [PreserveSig] int Reset();
    [PreserveSig] int SetEventHandle(IntPtr eventHandle);
    [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
}

public static class Probe {
    static string H(int hr) { return "0x" + ((uint)hr).ToString("X8"); }

    public static string Run(string targetId) {
        var sb = new StringBuilder();
        var enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(typeof(MMDeviceEnumerator));

        IMMDeviceCollection col;
        int hr = enumerator.EnumAudioEndpoints(EDataFlow.eRender, 1, out col);
        sb.AppendLine("EnumAudioEndpoints hr=" + H(hr));
        uint count; col.GetCount(out count);
        sb.AppendLine("active render endpoints: " + count);

        IMMDevice device = null;
        for (uint i = 0; i < count; i++) {
            IMMDevice d; col.Item(i, out d);
            string id; d.GetId(out id);
            sb.AppendLine("  [" + i + "] " + id);
            if (id == targetId) device = d;
        }
        if (device == null) { sb.AppendLine("TARGET NOT FOUND: " + targetId); return sb.ToString(); }
        sb.AppendLine("target matched.");

        // Also report the current default endpoint (all roles).
        foreach (ERole role in new[] { ERole.eConsole, ERole.eMultimedia, ERole.eCommunications }) {
            IMMDevice def;
            int dhr = enumerator.GetDefaultAudioEndpoint(EDataFlow.eRender, role, out def);
            if (dhr == 0 && def != null) {
                string did; def.GetId(out did);
                sb.AppendLine("default " + role + " = " + did);
            } else {
                sb.AppendLine("default " + role + " = (hr=" + H(dhr) + ")");
            }
        }

        Guid iid = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        object o;
        hr = device.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o);
        sb.AppendLine("Activate(IAudioClient) hr=" + H(hr));
        if (hr != 0 || o == null) return sb.ToString();
        var client = (IAudioClient)o;

        IntPtr pfmt;
        hr = client.GetMixFormat(out pfmt);
        sb.AppendLine("GetMixFormat hr=" + H(hr));
        if (hr == 0 && pfmt != IntPtr.Zero) {
            var fmt = (WAVEFORMATEXTENSIBLE)Marshal.PtrToStructure(pfmt, typeof(WAVEFORMATEXTENSIBLE));
            sb.AppendLine(string.Format(
                "MixFormat: tag={0} ch={1} rate={2} bits={3} valid={4} blockAlign={5} avgBytes={6} cbSize={7} mask=0x{8:X8} sub={9}",
                fmt.Format.wFormatTag, fmt.Format.nChannels, fmt.Format.nSamplesPerSec,
                fmt.Format.wBitsPerSample, fmt.wValidBitsPerSample, fmt.Format.nBlockAlign,
                fmt.Format.nAvgBytesPerSec, fmt.Format.cbSize, fmt.dwChannelMask, fmt.SubFormat));
        }

        long dp, mp;
        hr = client.GetDevicePeriod(out dp, out mp);
        sb.AppendLine("GetDevicePeriod hr=" + H(hr) + " default=" + dp + " min=" + mp);

        hr = client.Initialize(0, 0, 10000000, 0, pfmt, IntPtr.Zero);
        sb.AppendLine("Initialize(shared) hr=" + H(hr));

        uint frames;
        hr = client.GetBufferSize(out frames);
        sb.AppendLine("GetBufferSize hr=" + H(hr) + " frames=" + frames);

        hr = client.Start();
        sb.AppendLine("Start hr=" + H(hr));
        System.Threading.Thread.Sleep(2000);
        hr = client.Stop();
        sb.AppendLine("Stop hr=" + H(hr));
        return sb.ToString();
    }
}
}
'@

if (-not ('T2Wasapi.Probe' -as [type])) {
    Add-Type -TypeDefinition $code
}

[T2Wasapi.Probe]::Run($DeviceId)
