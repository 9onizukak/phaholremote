# Phahol Remote

ต้นแบบ Remote Desktop แบบ peer-to-peer สำหรับ Windows 10/11 x64 ใช้ C++17 และ Win32 โปรแกรมทั้งสองฝั่งใช้ไฟล์เดียวกันคือ `PhaholRemote.exe` แต่ละโปรเซสเปิด Host หนึ่งเซสชันและ Viewer หนึ่งเซสชันได้ ใช้ IPv4 ภายใน LAN และจับภาพจอหลักจากเดสก์ท็อปของผู้ใช้ที่เปิดแอป

## ## ## ติดตั้งและเปิดใช้

1. เปิด `PhaholRemote-Setup.exe` บนแต่ละเครื่องแล้วอนุญาต UAC หากกำลังอัปเดต ให้ปิด Phahol Remote ที่รันอยู่ก่อน
2. ตรวจรายการในหน้า **Trusted networks** แล้วกด **Install** ตัวติดตั้งจะคัดลอกโปรแกรมไปที่ Program Files สร้างทางลัด Start menu และ Startup แล้วเพิ่มกฎ Windows Firewall สำหรับโปรไฟล์ Private และ Domain ตามรายการ subnet
3. เลือก **Open Phahol Remote** ตอนกด Finish หรือเปิดจาก Start menu ครั้งถัดไปโปรแกรมจะเปิดใน system tray ข้างนาฬิกาอัตโนมัติหลังล็อกอิน Windows
4. ถอนการติดตั้งได้จาก **Settings → Apps → Installed apps → Phahol Remote** ตัวถอนการติดตั้งจะลบทางลัด Startup และกฎ Firewall ของโปรแกรมด้วย

หากใช้ชุด ZIP เดิม ให้เปิด Windows PowerShell แบบ Administrator ในโฟลเดอร์ที่แตกไฟล์ และรัน `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1` สำหรับถอนการติดตั้งจาก ZIP ให้เติม `-Uninstall`

Build ด้วย MinGW-w64 g++

ติดตั้ง [MSYS2](https://www.msys2.org/) แล้วเปิด **MSYS2 UCRT64** หากใช้ MinGW-w64 x64 ที่มีอยู่แล้ว ให้ตรวจว่า `g++ --version` ทำงานและใช้ชุดคอมไพเลอร์ที่รองรับ `std::thread` เช่น รุ่น POSIX thread

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
cd /h/LOY/phahol-remote
mkdir -p build
windres -I . -i src/phahol-remote.rc -o build/phahol-remote-icon.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 \
  -static -mwindows src/main.cpp build/phahol-remote-icon.o -o build/PhaholRemote.exe \
  -lws2_32 -lgdiplus -lgdi32 -lole32 -luser32 -lbcrypt -liphlpapi -luxtheme -lshell32 -luuid -ladvapi32 -lwinhttp -lruntimeobject
```

`-static` ลิงก์ runtime ของคอมไพเลอร์เข้า EXE ส่วน GDI+, Winsock, BCrypt และส่วนประกอบ Win32 มาจาก Windows

ทดสอบตัวประกอบเฟรมและการปรับคุณภาพ JPEG ได้ด้วยคำสั่งนี้

```bash
g++ -std=c++17 -O2 -Wall -Wextra -static tests/protocol_test.cpp -o build/protocol_test.exe
./build/protocol_test.exe
```

## Build ด้วย MSVC

ติดตั้ง **Desktop development with C++** จาก Visual Studio หรือ Build Tools พร้อม MSVC x64 และ Windows SDK เปิด **x64 Native Tools Command Prompt for VS** แล้วรันคำสั่งด้านล่าง [คู่มือ Microsoft สำหรับการคอมไพล์ผ่าน Command Prompt](https://learn.microsoft.com/en-us/cpp/build/walkthrough-compiling-a-native-cpp-program-on-the-command-line)

```bat
cd /d H:\LOY\phahol-remote
if not exist build mkdir build
rc /nologo /i . /fo build\phahol-remote-icon.res src\phahol-remote.rc
cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
  /Fo:build\main.obj /Fe:build\PhaholRemote.exe src\main.cpp build\phahol-remote-icon.res ^
  /link /SUBSYSTEM:WINDOWS ws2_32.lib gdiplus.lib gdi32.lib ole32.lib user32.lib bcrypt.lib iphlpapi.lib uxtheme.lib shell32.lib uuid.lib advapi32.lib winhttp.lib runtimeobject.lib
```

`/MT` ลิงก์ MSVC runtime เข้า EXE ใช้ `/SUBSYSTEM:WINDOWS` สำหรับ GUI แบบ Win32

```bat
cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /Fo:build\protocol_test.obj /Fe:build\protocol_test.exe tests\protocol_test.cpp
build\protocol_test.exe
```

## ## Build ตัวติดตั้ง EXE

บิลด์ `build/PhaholRemote.exe` ก่อน แล้วใช้ Inno Setup 7 คอมไพล์ `Setup.iss` จากรากโปรเจกต์ [ดาวน์โหลด Inno Setup](https://jrsoftware.org/isdl.php)

```powershell
ISCC.exe .\Setup.iss
```

ไฟล์ที่ได้คือ `build/PhaholRemote-Setup.exe`

## อัปเดตจาก GitHub Releases

ตั้งแต่รุ่น 0.1.25 โปรแกรมตรวจ GitHub Releases เมื่อเปิดและทุก 24 ชั่วโมง กด **Check for updates** ในหน้าต่างหลักหรือเมนูข้างนาฬิกาเพื่อตรวจทันที โปรแกรมดาวน์โหลดรุ่นใหม่ผ่าน HTTPS ตรวจ SHA-256 แล้วเริ่มติดตั้งอัตโนมัติเมื่อไม่มีเซสชัน Host หรือ Viewer Windows อาจแสดง UAC เพื่ออนุมัติสิทธิ์ติดตั้ง เมื่อเสร็จแล้วโปรแกรมเปิดกลับใน system tray และใช้ subnet, Firewall และสิทธิ์สั่งปิดเครื่องตามการตั้งค่าเดิม ให้ติดตั้งรุ่น 0.1.25 ก่อนเพื่อใช้การติดตั้งอัตโนมัติในรุ่นถัดไป

สำหรับ repo แบบ Private ให้ผู้ดูแลสร้าง fine-grained GitHub token จำกัดเฉพาะ `9onizukak/phaholremote` และให้ **Contents: Read-only** จากนั้นในบัญชี Windows ที่รันแอป เปิด **Credential Manager → Windows Credentials → Add a generic credential** ตั้งชื่อ `PhaholRemote/GitHub/9onizukak/phaholremote` ใส่ชื่อบัญชี GitHub ในช่อง User name และใส่ token ในช่อง Password ตัวตรวจอัปเดตอ่าน token จาก Windows Credential Manager และใช้กับ GitHub API เท่านั้น

การเผยแพร่รุ่นใหม่สำหรับตัวอัปเดต

1. เปลี่ยนรุ่นใน `src/version.h` แล้วบิลด์โปรแกรมและ Setup
2. รัน `powershell.exe -NoProfile -File .\Prepare-UpdateRelease.ps1` หลังบิลด์และลงนามไฟล์ Setup หากมีการลงนาม
3. สร้าง GitHub Release แบบ stable ที่ tag `v<version>` หรือ `<version>` ใน [9onizukak/phaholremote](https://github.com/9onizukak/phaholremote/releases) แนบ `PhaholRemote-Setup.exe` จากโฟลเดอร์ `build/release-v<version>` แล้วเผยแพร่เป็น Latest release ตัวตรวจอัปเดตใช้ค่า SHA-256 ที่ GitHub ระบุในข้อมูลไฟล์แนบ

## เชื่อมต่อข้าม VLAN ผ่าน L3 switch

ตัวติดตั้งกำหนดรายการเริ่มต้นเป็น `192.168.0.0/16,172.16.0.0/16` ใช้ตัวติดตั้งรุ่นเดียวกันทั้ง Host และ Client หน้า **Trusted networks** รับ IPv4 CIDR คั่นด้วยจุลภาค หากต้องการอนุญาตแค่บางเครื่อง ให้ใช้ IP ของแต่ละเครื่องตามด้วย `/32` หากเว้นว่าง แอปรับเฉพาะ subnet ที่ต่ออยู่โดยตรง

- ฝั่ง Host ต้องอนุญาต subnet ของ Client เพื่อรับ TCP 53540 และฝั่ง Client ต้องอนุญาต subnet ของ Host เพื่อรับภาพ UDP 53541 ติดตั้งด้วยรายการเดียวกันได้หากต้องสลับบทบาท
- L3 switch ต้อง route ไปและกลับระหว่าง VLAN โดยเครื่องใช้ default gateway ของ VLAN ตัวเอง หากมี ACL ให้เปิด TCP จาก Client ไป Host ปลายทาง 53540 พร้อมทราฟฟิกตอบกลับ และ UDP จาก Host ไป Client ปลายทาง 53541 โดย UDP ต้นทางของ Host เป็นพอร์ตชั่วคราว
- จาก Client ใช้ `Test-NetConnection <IPv4-ของ-Host> -Port 53540` เพื่อตรวจ TCP ถ้าเชื่อมต่อแล้วภาพไม่มา ให้ตรวจ ACL ขากลับสำหรับ UDP 53541 และโปรไฟล์ Firewall บน Client
- โปรแกรมอ่าน `trusted-subnets.txt` จากโฟลเดอร์เดียวกับ EXE ตอนเปิด ตัวติดตั้งเขียนไฟล์นี้และตั้งกฎ Firewall ให้ตรงกัน หากต้องแก้รายการ ให้ปิดแอปแล้วรันตัวติดตั้งใหม่

ตั้งค่า Windows Firewall สำหรับการรัน EXE โดยตรง

หากเปิด EXE โดยตรง ให้รัน PowerShell แบบ Administrator ที่เครื่องที่ต้องทำหน้าที่นั้น โดยรันจากรากโปรเจกต์หรือเปลี่ยน `$exe` ให้ตรงกับพาธจริงของไฟล์ที่รัน กฎจำกัดไว้ที่โปรแกรมนี้ โปรไฟล์ **Private** และเครื่องใน subnet เดียวกัน คำสั่งแสดงการตั้งค่าสำหรับเครื่องที่สลับบทบาทได้ หากใช้บทบาทเดียวให้เพิ่มเฉพาะกฎของฝั่งนั้น [คู่มือ New-NetFirewallRule](https://learn.microsoft.com/en-us/powershell/module/netsecurity/new-netfirewallrule)

```powershell
$exe = (Resolve-Path .\build\PhaholRemote.exe).Path

# Host รับ handshake และคำสั่งที่ TCP 53540
New-NetFirewallRule -Name 'PhaholRemote-Host-TCP' -DisplayName 'Phahol Remote Host TCP' `
  -Direction Inbound -Action Allow -Profile Private -RemoteAddress LocalSubnet `
  -Program $exe -Protocol TCP -LocalPort 53540

# Viewer รับภาพที่ UDP 53541
New-NetFirewallRule -Name 'PhaholRemote-Viewer-UDP' -DisplayName 'Phahol Remote Viewer UDP' `
  -Direction Inbound -Action Allow -Profile Private -RemoteAddress LocalSubnet `
  -Program $exe -Protocol UDP -LocalPort 53541
```

Host ใช้พอร์ต UDP ต้นทางที่ระบบเลือก แล้วแจ้ง Viewer ผ่าน handshake ตัวอย่างนี้ใช้ค่า outbound ปกติของ Windows ถ้าต้องการจำกัดเฉพาะอีกเครื่อง ให้แทน `LocalSubnet` ด้วย IPv4 ของเครื่องนั้น

```powershell
Remove-NetFirewallRule -Name 'PhaholRemote-Host-TCP','PhaholRemote-Viewer-UDP'
```

## ทดลองใช้

1. บนเครื่องที่จะให้ควบคุม ให้โปรแกรมเปิดอยู่และดู IPv4 ของ LAN ด้วย `ipconfig` Host เปิดรับการเชื่อมต่อเองเมื่อโปรแกรมเริ่มทำงาน ตั้งแต่รุ่น 0.1.17 ให้ดู IPv4 ได้จากป้ายที่มุมขวาบนของจอหลัก ป้ายเลือก IPv4 ของ Ethernet หรือ Wi-Fi ที่ใช้งานอยู่และอัปเดตทุก 5 วินาที ลากป้ายเพื่อย้ายตำแหน่ง หรือคลิกขวาที่ป้ายแล้วเลือก **Hide IPv4 badge** เปิดกลับได้จาก **Show IPv4 badge** ในเมนูไอคอน Phahol Remote ข้างนาฬิกา
2. บนเครื่องที่ใช้ควบคุม กรอก IPv4 ปลายทางในช่อง **Target IPv4** แล้วกด **Connect** หรือ Enter หน้าต่าง Viewer จะเปิดแบบขยายเมื่อเชื่อมต่อสำเร็จ ใช้รุ่น 0.1.5 ขึ้นไปทั้งสองฝั่งเพื่อรับภาพ Full HD
3. เมื่อภาพปรากฏ ให้คลิกในภาพเพื่อรับโฟกัส แล้วทดสอบพิมพ์ใน Notepad ที่เปิดบนปลายทางหรือทดสอบเมาส์และล้อเลื่อน
4. กด **Disconnect** หรือปิด Viewer เพื่อจบเซสชัน หากต้องการหยุดรับการเชื่อมต่อบนเครื่องปลายทาง ให้ปิดหน้าต่างหลักของโปรแกรมบนเครื่องนั้น กดปุ่มย่อที่หน้าต่างหลักเพื่อเก็บไว้ใน system tray โดยยังเปิดรับการเชื่อมต่อ คลิกไอคอนเพื่อเปิดหน้าต่างกลับ หรือคลิกขวาแล้วเลือก **Exit** เพื่อปิดโปรแกรม ถ้าไอคอนอยู่ในรายการที่ซ่อนไว้ให้กดลูกศรข้างนาฬิกา

ใช้ `127.0.0.1` สำหรับลองจับภาพ ส่งภาพ และตรวจ RTT ในโปรเซสเดียว ควรทดสอบการควบคุมจริงบนสองเครื่อง เพราะ loopback จับภาพ Viewer ที่อยู่บนจอเดียวกันด้วย

## ตรวจสอบเมาส์เมื่อเห็นภาพแต่คลิกไม่ได้

อัปเดตทั้งสองฝั่งเป็นรุ่น 0.1.9 ขึ้นไป เปิด Notepad แบบปกติบน Host แล้วลองขยับและคลิกจาก Viewer ตั้งแต่รุ่น 0.1.12 Viewer ใช้ลูกศรของ Windows บนเครื่อง Viewer เพื่อให้ตัวชี้ขยับได้ทันทีแม้ภาพจาก Host หยุดชั่วคราว แถบชื่อจะแสดง **Host accepted click** เมื่อ Windows รับคำสั่งเข้าคิว หรือ **Host Windows blocked click** เมื่อ Windows ไม่รับคลิก หรือ **Host cursor position could not be confirmed** เมื่อ Host ยืนยันตำแหน่งตัวชี้ไม่ได้ รุ่น 0.1.13 ยังแยก **Host input desktop unavailable** เมื่อเดสก์ท็อปสำหรับรับ input เข้าไม่ถึง และ **Host application restricted cursor movement** เมื่อตำแหน่งอยู่นอกพื้นที่ที่ Windows อนุญาต ตรวจว่า Host อยู่บนเดสก์ท็อปที่ปลดล็อกแล้ว การควบคุมหน้าต่างที่เปิดด้วยสิทธิ์สูงกว่า Host ถูกจำกัดโดย UIPI [ข้อจำกัดของ SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)

## ## คำสั่งระบบบน Host

ใน Viewer รุ่น 0.1.15 ขึ้นไป เปิดเมนู **Host** เพื่อเลือก **Ctrl+Alt+Del**, **Shut down** หรือ **Restart** ใช้รุ่นนี้ทั้งสองเครื่องเพื่อส่งคำสั่งระบบผ่านเซสชันเดิม

- สำหรับ **Shut down** และ **Restart** ให้เลือก **Allow connected Viewers to shut down or restart this Host** ในตัวติดตั้งของ Host ค่าเริ่มต้นปิดไว้ หลัง Viewer ยืนยัน Windows จะได้รับคำขอให้ปิดเครื่องหรือรีสตาร์ตใน 15 วินาที โดยอนุญาตให้แอปแจ้งเตือนเรื่องงานที่ยังไม่ได้บันทึก
- **Ctrl+Alt+Del** ใช้ Windows Secure Attention Sequence รุ่นที่แจกจ่ายอยู่ต้องใช้ Host ที่ลงนามแบบ Authenticode และเปิด UIAccess พร้อมนโยบาย Windows ที่อนุญาต SAS จึงจะส่งคำสั่งได้ [ข้อกำหนด Windows SendSAS](https://learn.microsoft.com/en-us/windows/win32/api/sas/nf-sas-sendsas)

## ส่งไฟล์จาก Viewer ไป Host

1. ติดตั้ง Phahol Remote 0.1.22 บน Viewer เปิดแอปจาก Start menu แล้วเชื่อมต่อกับ Host รุ่น 0.1.21 หรือ 0.1.22
2. กด **Send files...** ที่ด้านบนของ Viewer เพื่อเลือกไฟล์จากเครื่อง Viewer หรือลากไฟล์จาก File Explorer มาวางในหน้าต่าง Viewer อีกวิธีคือกด Ctrl+C ที่ไฟล์ใน Explorer แล้วกด Ctrl+V ใน Viewer หรือกด **Paste files** หาก Viewer ขยายเต็มจอ ให้กด Restore เพื่อวางสองหน้าต่างข้างกัน
3. ดูสถานะการส่งที่แถบด้านบนของ Viewer เมื่อ Host ยืนยันว่าบันทึกแล้ว จะขึ้นจำนวนไฟล์ที่บันทึกใน **Downloads → Phahol Remote** ของผู้ใช้บนเครื่อง Host

ส่งไฟล์ได้หลายไฟล์ ขนาดสูงสุด 1 GiB ต่อไฟล์ และคิวรอสูงสุด 32 ไฟล์ เมื่อชื่อซ้ำ Host เติมเลขต่อท้าย เช่น `report (1).pdf` ไฟล์ที่ส่งไม่ครบจะถูกล้างเมื่อเซสชันสิ้นสุดตามปกติ การส่งใช้ TCP 53540 เดิม สลับข้อมูลไฟล์ขนาดสูงสุด 32 KiB กับคำสั่งควบคุมและ feedback

## Wire format

ทุกฟิลด์ด้านล่างเข้ารหัสเป็น unsigned 32-bit แบบ big-endian ทีละฟิลด์ การจัดเรียงหน่วยความจำของ C++ struct จึงไม่ผูกกับรูปแบบบนเครือข่าย

| โครงสร้าง | ฟิลด์ตามลำดับบนสาย | ขนาด |
| --- | --- | --- |
| `UdpChunkHeader` | `magic`, `sessionHigh`, `sessionLow`, `frameId`, `jpegBytes`, `chunkIndex`, `chunkCount` | 28 ไบต์ ตามด้วยข้อมูล JPEG สูงสุด 1,200 ไบต์ |
| `InputPacket` | `kind`, `x`, `y`, `value`, `flags` | 20 ไบต์ |
| `FeedbackPacket` | `ackFrameId`, `decodedFrames`, `droppedFrames`, `rttMs` | 16 ไบต์ |

- **UDP** ใช้ magic `PHR1` และ session ID สุ่ม 64 บิตจาก handshake พอร์ตต้นทางและ IP ถูกตรวจผ่าน connected UDP socket ส่วน header, ขนาดชิ้น และ session ID ถูกตรวจในแอป `frameId` เริ่มที่ 1 และ `chunkIndex` เริ่มที่ 0 แต่ละแพ็กเก็ตมีขนาดสูงสุด 1,256 ไบต์รวม IPv4 header ปกติและ UDP header
- **TCP** เริ่มแต่ละข้อความด้วย type ขนาด 4 ไบต์ แล้วตามด้วย payload ขนาดคงที่ตาม type `Hello(magic, version)` → `Welcome(version, sessionHigh, sessionLow, hostUdpPort)` → `Ready(version)` จากนั้นรับ `Input`, `Feedback` และ `Ping(id)` โดย Host ตอบ `Pong(id, lastAttemptedFrameId)` ฟังก์ชันอ่านและเขียนวนจนได้จำนวนไบต์ครบหรือครบ timeout โปรโตคอลรุ่น 3 เพิ่ม `FileBegin(size, nameBytes)` ตามด้วยชื่อ UTF-8, `FileChunk(size)` ตามด้วยข้อมูล, `FileEnd(commit)` และ `FileResult(status)` สำหรับยืนยันการรับและบันทึก โปรโตคอลรุ่น 4 เพิ่ม `MouseResult(kind, status)` เพื่อแจ้งผลการส่งเมาส์เข้า Windows เมื่อสถานะเปลี่ยน โปรโตคอลรุ่น 5 เพิ่มรายละเอียดสถานะเดสก์ท็อปและตำแหน่งตัวชี้
- **Input** ใช้พิกัดช่วง 0–65535 แทน 0.0–1.0 โดยคำนวณจากขอบภาพที่เรนเดอร์จริง `value` เก็บหมายเลขปุ่มเมาส์, wheel delta แบบ two's complement หรือ keyboard scan code ส่วน flags ระบุ down, extended key หรือ horizontal wheel ตามชนิดเหตุการณ์ Host ปล่อยปุ่มและคีย์ที่รีโมตกดค้างไว้เมื่อ Viewer เสียโฟกัสหรือเซสชันสิ้นสุด โดยส่งการปล่อยปุ่มแยกจากการย้ายตัวชี้ และลองปล่อยซ้ำระหว่างเซสชันหาก Windows ปฏิเสธครั้งแรก
- **Feedback** ส่งทุกประมาณ 500 ms `ackFrameId` คือเฟรมล่าสุดที่ถอด JPEG สำเร็จ ส่วนจำนวนเฟรมสำเร็จและเฟรมตกนับเฉพาะช่วงรายงานนั้น `rttMs` มาจาก TCP ping/pong ด้วย monotonic clock และใช้ `0xFFFFFFFF` ก่อนมีผลวัด ค่า drop เป็นสถิติระดับเฟรม

## การจับภาพ การทิ้งเฟรม และ Adaptive Quality

Host จับภาพด้วย `BitBlt` หรือ `StretchBlt` ลง DIB โดยรักษาสัดส่วนและจำกัดที่ 1920×1080 จากนั้นใช้ `Gdiplus::EncoderQuality` ตอน encode เฟรมใหม่ เป้าหมายประมาณ 15 FPS ขึ้นอยู่กับเวลาจับภาพ บีบอัด และส่งจริง

Viewer เก็บเฟรมที่ประกอบค้างได้หนึ่งเฟรม รับชิ้นที่สลับลำดับภายในเฟรมเดียวกันได้และข้ามชิ้นซ้ำ เมื่อชิ้นแรกของเฟรมที่ใหม่กว่ามาถึงหรือครบ 120 ms จะทิ้งเฟรมค้างทันที เฟรมที่ขาดทั้งเฟรมนับจากช่องว่างของ frame ID หากภาพหยุดส่งถึง Viewer ทั้งหมด Viewer ใช้ watermark จาก TCP โดยรออีกหนึ่งรอบ ping/pong ก่อนนำมานับ เพื่อให้ชิ้น UDP ที่ยังเดินทางอยู่มีเวลามาถึง `Decode` ทำงานกับ JPEG ที่ประกอบครบเท่านั้น และ UI ถือภาพที่ถอดสำเร็จล่าสุดไว้ หากไม่มีเฟรมใหม่เกินหนึ่งวินาที Viewer จะล้างภาพเก่าและพักการส่งเมาส์จนได้ภาพใหม่

| เงื่อนไข | การปรับ JPEG |
| --- | --- |
| เริ่มเซสชัน | 90% |
| RTT มากกว่า 70 ms หรือพบเฟรมตกสองรอบรายงานติดกัน | ลด 20 จุดต่อครั้ง ต่ำสุด 20% |
| มีภาพถอดสำเร็จ ไม่มีเฟรมตก และ RTT ไม่เกิน 50 ms ติดต่อกัน 6 รอบ | เพิ่ม 5 จุดทุกประมาณ 3 วินาที สูงสุด 90% |

Host TCP thread เป็นเจ้าของตัวนับใน `AdaptiveQuality` แล้วเขียนผลไปยัง `std::atomic<int>` ซึ่ง video thread อ่านหนึ่งครั้งก่อน encode แต่ละเฟรม ส่วน Viewer มี TCP thread ตัวเดียวที่ส่ง input, feedback และ ping จึงไม่มีการเขียนข้อความ TCP แทรกกัน mutex แยกกันป้องกันคิว input และข้อมูลภาพกับสถิติ การอัปเดตภาพส่งต่อผ่าน `shared_ptr<const Pixels>` และ UI เรนเดอร์บน Win32 thread

Host รับการเชื่อมต่อจาก IPv4 ใน subnet ที่ต่อโดยตรงหรืออยู่ในรายการ Trusted networks โดยอัตโนมัติ ผู้ที่เข้าถึงพอร์ตได้สามารถดูภาพและควบคุมเครื่องได้ด้วย IP เพียงอย่างเดียว การส่งข้อมูลใช้ plaintext จึงควรติดตั้งเฉพาะบน LAN ที่เชื่อถือได้ การจับภาพและควบคุมใช้เดสก์ท็อปของผู้ใช้ที่ล็อกอินอยู่ การฉีด input ไปยังโปรแกรมที่มี integrity level สูงกว่าถูกจำกัดโดย UIPI และ secure desktop อยู่นอกขอบเขตของต้นแบบ [ข้อจำกัดของ SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
