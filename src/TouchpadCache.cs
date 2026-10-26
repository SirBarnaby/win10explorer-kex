// Adapted from AppDock.at365 TouchpadWindowsCache.cs (MIT).
// Copyright (c) 2026 yukichi-7sai. See licenses/AppDock-MIT.txt.
// Changes: .NET Framework 4.8 compatibility, bounded Win10 profile, CLI/readback.
using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Web.Script.Serialization;
using Microsoft.Win32;

class TouchpadCache {
    const string PathKey = @"Software\Microsoft\Windows\CurrentVersion\PrecisionTouchPad";
    static readonly string[] Directions = { "Left", "Up", "Right", "Down" };
    static readonly string[] Names = Directions.SelectMany(d => new[] { "ThreeFinger" + d + "KeyParams", "ThreeFinger" + d }).Concat(new[] { "ThreeFingerSlideEnabled" }).ToArray();
    struct Segment { public int Offset, Size; public uint Rva; }
    class Image {
        public byte[] Bytes;
        public ulong Base;
        public List<Segment> Segments = new List<Segment>();
        public Image(string path) {
            Bytes = File.ReadAllBytes(path);
            if(Bytes.Length < 512 || U16(0) != 0x5a4d) throw new InvalidDataException("Invalid PE header");
            int pe=I32(0x3c);
            if(pe<64 || pe>Bytes.Length-264 || I32(pe)!=0x4550 || U16(pe+4)!=0x8664) throw new InvalidDataException("Unsupported PE machine");
            int n=U16(pe+6), opt=pe+24, sz=U16(pe+20), table=opt+sz;
            if(U16(opt)!=0x20b || n<1 || n>96 || sz<112 || table>Bytes.Length-n*40) throw new InvalidDataException("Unsupported PE layout");
            Base=U64(opt+24);
            for(int i=0;i<n;i++) { int at=table+i*40, len=I32(at+16), off=I32(at+20);
                if(len<0 || off<0 || off>Bytes.Length-len) throw new InvalidDataException("Section outside image");
                if(len>0) Segments.Add(new Segment { Offset=off, Size=len, Rva=unchecked((uint)I32(at+12)) });
            }
        }
        public ushort U16(int at) { return BitConverter.ToUInt16(Bytes,at); }
        public int I32(int at) { return BitConverter.ToInt32(Bytes,at); }
        public ulong U64(int at) { return BitConverter.ToUInt64(Bytes,at); }
        public bool Equal(int at,byte[] value) { for(int i=0;i<value.Length;i++) if(Bytes[at+i]!=value[i]) return false; return true; }
        public HashSet<ulong> Addresses(string name) {
            byte[] needle=Encoding.Unicode.GetBytes(name+'\0'); var result=new HashSet<ulong>();
            foreach(var s in Segments) for(int at=s.Offset;at<=s.Offset+s.Size-needle.Length;at+=2)
                if(Equal(at,needle)) result.Add(Base+s.Rva+(ulong)(at-s.Offset));
            return result;
        }
        public string Hash() { using(var hash=SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(Bytes)).Replace("-","").ToLowerInvariant(); }
    }
    static Dictionary<string,int> Profile(out string hash) {
        var image=new Image(System.IO.Path.Combine(Environment.SystemDirectory,"twinui.dll"));
        hash=image.Hash(); var paths=image.Addresses(PathKey); var ids=new Dictionary<string,int>();
        if(paths.Count==0) throw new InvalidDataException("Touchpad path absent from Windows settings cache");
        foreach(string name in Names) {
            var addresses=image.Addresses(name); var matches=new HashSet<int>();
            foreach(var s in image.Segments) for(int at=s.Offset+16;at<=s.Offset+s.Size-24;at+=8) {
                if(!addresses.Contains(image.U64(at)) || !paths.Contains(image.U64(at-8))) continue;
                ulong id=image.U64(at-16);
                if(id>0 && id<256 && image.U64(at+8)<=uint.MaxValue && image.U64(at+16)==0) matches.Add((int)id);
            }
            if(matches.Count!=1) throw new InvalidDataException("Unsupported/ambiguous cache descriptor for "+name);
            ids.Add(name,matches.Single());
        }
        if(ids.Values.Distinct().Count()!=Names.Length) throw new InvalidDataException("Duplicate cache setting IDs");
        var handler=new Image(System.IO.Path.Combine(Environment.SystemDirectory,"SettingsHandlers_nt.dll"));
        foreach(string d in Directions) {
            var keys=handler.Addresses("ThreeFinger"+d+"KeyParams");
            var actions=handler.Addresses("ThreeFinger"+d);
            var modes=handler.Addresses("ThreeFingerSlideEnabled"); int matches=0;
            foreach(var s in handler.Segments) for(int at=s.Offset;at<=s.Offset+s.Size-40;at+=8)
                if(keys.Contains(handler.U64(at)) && actions.Contains(handler.U64(at+8)) && unchecked((uint)handler.I32(at+20))==0xffff && modes.Contains(handler.U64(at+24)) && handler.I32(at+32)==0xffff) matches++;
            if(matches!=1) throw new InvalidDataException("Unsupported/ambiguous custom gesture descriptor for "+d);
        }
        return ids;
    }
    [ComImport,Guid("6d5140c1-7436-11ce-8034-00aa006009fa"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IServiceProviderNative { [PreserveSig] int QueryService(ref Guid service,ref Guid iid,[MarshalAs(UnmanagedType.Interface)] out IImmersiveSettingsCache result); }
    [ComImport,Guid("4214f6fa-eb36-4e2f-9ca2-23fdc1832df7"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IImmersiveSettingsCache {
        [PreserveSig] int OnSettingChanged(int setting);
        [PreserveSig] int GetBOOL(int setting,[MarshalAs(UnmanagedType.Bool)] out bool value);
        [PreserveSig] int GetDWORD(int setting,out uint value);
        [PreserveSig] int RegisterForSettingChange(IntPtr listener,out uint cookie);
        [PreserveSig] int UnregisterForSettingChange(uint cookie);
    }
    [STAThread] static int Main(string[] args) {
        object shell=null; IImmersiveSettingsCache cache=null;
        try {
            if(args.Length!=1 || (args[0]!="--probe" && args[0]!="--refresh")) throw new ArgumentException("Use --probe or --refresh");
            string hash; var ids=Profile(out hash);
            shell=Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("c2f03a33-21f5-47fa-b4bb-156362a2f239"),true));
            var provider=(IServiceProviderNative)shell; var service=new Guid("53660488-8855-460b-a9ab-5cfc6b5012ca"); var iid=typeof(IImmersiveSettingsCache).GUID;
            Marshal.ThrowExceptionForHR(provider.QueryService(ref service,ref iid,out cache));
            var readback=new Dictionary<string,uint>();
            using(var key=Registry.CurrentUser.OpenSubKey(PathKey)) {
                if(key==null) throw new InvalidDataException("No Precision Touchpad settings on this PC");
                foreach(string name in Names) {
                    if(args[0]=="--refresh") Marshal.ThrowExceptionForHR(cache.OnSettingChanged(ids[name]));
                    uint actual; Marshal.ThrowExceptionForHR(cache.GetDWORD(ids[name],out actual)); readback[name]=actual;
                    object expected=key.GetValue(name);
                    // Absent values deliberately restore Windows defaults during uninstall.
                    if(args[0]=="--refresh" && expected!=null && (key.GetValueKind(name)!=RegistryValueKind.DWord || unchecked((uint)(int)expected)!=actual)) throw new InvalidDataException("Cache readback differs for "+name);
                }
            }
            Console.WriteLine(new JavaScriptSerializer().Serialize(new { ok=true, refreshed=args[0]=="--refresh", twinuiSha256=hash, ids=ids, effective=readback })); return 0;
        } catch(Exception e) { Console.WriteLine(new JavaScriptSerializer().Serialize(new { ok=false, error=e.Message, fallback="Sign out and sign in to reload touchpad settings." })); return 2; }
        finally { if(cache!=null && Marshal.IsComObject(cache)) Marshal.ReleaseComObject(cache); if(shell!=null && Marshal.IsComObject(shell)) Marshal.ReleaseComObject(shell); }
    }
}
