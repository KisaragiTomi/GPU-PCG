; Roof_TerraceAndRidge.md「屋面瓦资产（2026-09-29）」的反汇编证据
; 来源：D:/MyProject/Tiny Glade/tiny-glade.exe（SHA256 cf048e27acdc266b2efc7c094dd435bacecb3f26c2afe54de4d09cd3f7036323）+ tiny_glade.pdb，
;       dumpbin /DISASM:NOBYTES 全量流式过滤。__real@xxxxxxxx 是单精度字面量（TG 米制），__xmm@ 按高位在前写。
;
; 读法速查（地址都在下面的正文里）：
; 坡面瓦（逐排）
;   1421B0F37  divss 坡长, 0.4375 → ceilf → max 2 = N；排线 u_i = i/(N-1)，i = 0..N-2（见文末排线闭包 1419F0850..）
;   1421B1168  addss 排长, 排长 → ceilf → max 3 = 每排片数；1421B11C9 random_splits(片数+1 个分点, 抖动 0) ⇒ 同排等宽
;   1421B13DE  mulss 排距, 1.4285714 (=1/0.7) → 瓦长基数
;   1421B169F  (1-r)*0.8 与 r*1.2 → 1421B1A87 相加再乘瓦长基数 ⇒ 瓦长 = 排距/0.7 × (0.8 + 0.4r)
;   1421B1AAE  mov [rbp+438h],3DCCCCCDh ⇒ scale_t.z = 0.1 m（板厚字面常数）；scale_t.x = 两分点间距（瓦宽）
;   1421B18AB  u0 > 0.5 选左 / 右两个角斜脊裁剪面之一写进 clipping_plane_
;   1421B1B13  is_main_roof_body：首排（或函数第 3 参数为真）记 2；标志位 [rbp+120h]&1 成立时离排端 < 0.5 m 也记 2；
;              其余 1。VS 只判 != 0，所以 1 / 2 都做瓦端抬升
; 角斜脊（4 条，1421AF1AC 起 rsi = 0..3）
;   1421AF386  /0.625 → roundf → max 2 = 分点数；1421AF3DC 抖动 0.125/L；1421AF6EB 段长 + 0.05
;   1421AFAE0  scale = (0.25, 段长+0.05, 0.18 [3E3851ECh])；裁剪面 half4 7BFF00003C000000h = (0,1,0,65504) 即不裁
;   1421AF8DD / 1421AF8E5 法线项乘 -0.05 m；1421AFB24 / 1421AFB2D 加到位置中，窄板下沉嵌入两坡的对切口
; 水平屋脊（RoofRidgeDims::to_world_space 返回 1 时）
;   1421AFF99  /0.625 → roundf → max 2；1421AFFEE 抖动 0.125/L
;   1421B020B  宽 = 0.4 + 0.15·smoothstep(0.8, 1, 1 - a)（a = 屋顶参数 [+2Ch]·[+30h]）
;   1421B028C  长 = 段长 × 1.4；1421B02AA 厚 3E19999Ah = 0.15；首尾两块 is_main_roof_body = 2
; utils::random_splits_into（140C90050）：n 个分点 0, k/(n-1) + (rand-0.5)·j, …, 1；j = min(抖动, 0.495/(n-1))

; ==================================================================
; system_roof::utils::assemble_rectangular_roof（全文）
; _ZN11system_roof5utils25assemble_rectangular_roof25assemble_rectangular_roof17hb107ea4b47d5a245E
  00000001421AEEF0: push        rbp
  00000001421AEEF1: push        r15
  00000001421AEEF3: push        r14
  00000001421AEEF5: push        r13
  00000001421AEEF7: push        r12
  00000001421AEEF9: push        rsi
  00000001421AEEFA: push        rdi
  00000001421AEEFB: push        rbx
  00000001421AEEFC: sub         rsp,5B8h
  00000001421AEF03: lea         rbp,[rsp+80h]
  00000001421AEF0B: movaps      xmmword ptr [rbp+520h],xmm15
  00000001421AEF13: movaps      xmmword ptr [rbp+510h],xmm14
  00000001421AEF1B: movaps      xmmword ptr [rbp+500h],xmm13
  00000001421AEF23: movaps      xmmword ptr [rbp+4F0h],xmm12
  00000001421AEF2B: movaps      xmmword ptr [rbp+4E0h],xmm11
  00000001421AEF33: movaps      xmmword ptr [rbp+4D0h],xmm10
  00000001421AEF3B: movaps      xmmword ptr [rbp+4C0h],xmm9
  00000001421AEF43: movaps      xmmword ptr [rbp+4B0h],xmm8
  00000001421AEF4B: movaps      xmmword ptr [rbp+4A0h],xmm7
  00000001421AEF52: movaps      xmmword ptr [rbp+490h],xmm6
  00000001421AEF59: mov         qword ptr [rbp+488h],0FFFFFFFFFFFFFFFEh
  00000001421AEF64: mov         byte ptr [rbp+487h],r8b
  00000001421AEF6B: mov         rdi,rdx
  00000001421AEF6E: mov         qword ptr [rbp+3E8h],rcx
  00000001421AEF75: mov         qword ptr [rbp+3A8h],0
  00000001421AEF80: lea         rsi,[rbp+430h]
  00000001421AEF87: mov         rcx,rsi
  00000001421AEF8A: call        _ZN12country_core9resources5roofs17roof_visual_shape32get_roof_expanded_top_and_bottom17h49013d2de44c42a5E
  00000001421AEF8F: lea         rcx,[rbp+338h]
  00000001421AEF96: mov         rdx,rsi
  00000001421AEF99: call        _ZN12country_core9resources5roofs17roof_visual_shape14RoofBoundsDims12as_rectangle17hb0e2c674592d4300E
  00000001421AEF9E: movss       xmm6,dword ptr [rbp+338h]
  00000001421AEFA6: movss       xmm7,dword ptr [rbp+33Ch]
  00000001421AEFAE: movss       xmm0,dword ptr [rbp+340h]
  00000001421AEFB6: movaps      xmmword ptr [rbp+170h],xmm0
  00000001421AEFBD: movss       xmm8,dword ptr [rbp+344h]
  00000001421AEFC6: lea         rcx,[rbp+190h]
  00000001421AEFCD: mov         qword ptr [rbp+428h],rdi
  00000001421AEFD4: mov         rdx,rdi
  00000001421AEFD7: call        _ZN12country_core9resources5roofs10roof_shape4Roof24profile_curve_normalized17h6682cbbbfd041ff0E
  00000001421AEFDC: lea         r9,[142FF80C0h]
  00000001421AEFE3: lea         rcx,[rbp+0A8h]
  00000001421AEFEA: movaps      xmm1,xmm6
  00000001421AEFED: movaps      xmm2,xmm7
  00000001421AEFF0: call        _ZN5utils8geometry9rectangle11Rectangle2d25new_centered_axis_aligned17h88232fc6a85fc4acE
  00000001421AEFF5: xorps       xmm1,xmm1
  00000001421AEFF8: xorps       xmm2,xmm2
  00000001421AEFFB: mov         rcx,qword ptr [rbp+428h]
  00000001421AF002: call        _ZN12country_core9resources5roofs10roof_ridge23calc_roof_tip_center_ls17h41f7de4b1f1b8a19E
  00000001421AF007: movaps      xmmword ptr [rbp+180h],xmm0
  00000001421AF00E: movaps      xmmword ptr [rbp+160h],xmm1
  00000001421AF015: lea         rax,[142FF80D8h]
  00000001421AF01C: mov         qword ptr [rsp+28h],rax
  00000001421AF021: movaps      xmm0,xmmword ptr [rbp+160h]
  00000001421AF028: movss       dword ptr [rsp+20h],xmm0
  00000001421AF02E: lea         rcx,[rbp+148h]
  00000001421AF035: movaps      xmm1,xmmword ptr [rbp+170h]
  00000001421AF03C: movaps      xmm2,xmm8
  00000001421AF040: movaps      xmm3,xmmword ptr [rbp+180h]
  00000001421AF047: call        _ZN5utils8geometry9rectangle11Rectangle2d16new_axis_aligned17hd4a7833d38f87225E
  00000001421AF04C: lea         rcx,[rbp+110h]
  00000001421AF053: mov         rdx,qword ptr [rbp+3E8h]
  00000001421AF05A: call        _ZN5utils8geometry9rectangle11Rectangle2d16orientation_mat217h578587259d3307efE
  00000001421AF05F: mov         rcx,qword ptr [rbp+428h]
  00000001421AF066: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  00000001421AF06B: test        al,al
  00000001421AF06D: je          00000001421AF0B4
  00000001421AF06F: mov         rax,qword ptr [rbp+428h]
  00000001421AF076: movzx       ebx,byte ptr [rax+54h]
  00000001421AF07A: test        bl,bl
  00000001421AF07C: jne         00000001421AF0BE
  00000001421AF07E: movss       xmm6,dword ptr [__real@3f4f5c29]
  00000001421AF086: mov         rax,qword ptr [rbp+428h]
  00000001421AF08D: lea         rcx,[rax+24h]
  00000001421AF091: cmp         byte ptr [rax+3Ch],0
  00000001421AF095: jne         00000001421AF0D7
  00000001421AF097: call        _ZN12country_core9resources5roofs10roof_shape15RoofShapeParams14eave_length_ws17h40916087298468ffE
  00000001421AF09C: lea         rax,[rbp+0B8h]
  00000001421AF0A3: lea         rcx,[rbp+158h]
  00000001421AF0AA: addss       xmm6,xmm0
  00000001421AF0AE: test        bl,bl
  00000001421AF0B0: je          00000001421AF0F2
  00000001421AF0B2: jmp         00000001421AF0F9
  00000001421AF0B4: mov         qword ptr [rbp-10h],0
  00000001421AF0BC: jmp         00000001421AF115
  00000001421AF0BE: movss       xmm6,dword ptr [__real@3e800000]
  00000001421AF0C6: mov         rax,qword ptr [rbp+428h]
  00000001421AF0CD: lea         rcx,[rax+24h]
  00000001421AF0D1: cmp         byte ptr [rax+3Ch],0
  00000001421AF0D5: je          00000001421AF097
  00000001421AF0D7: call        _ZN12country_core9resources5roofs10roof_shape15RoofShapeParams14eave_length_ws17h40916087298468ffE
  00000001421AF0DC: lea         rax,[rbp+0BCh]
  00000001421AF0E3: lea         rcx,[rbp+15Ch]
  00000001421AF0EA: addss       xmm6,xmm0
  00000001421AF0EE: test        bl,bl
  00000001421AF0F0: jne         00000001421AF0F9
  00000001421AF0F2: xorps       xmm6,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421AF0F9: addss       xmm6,dword ptr [rax]
  00000001421AF0FD: movss       dword ptr [rax],xmm6
  00000001421AF101: movss       dword ptr [rcx],xmm6
  00000001421AF105: lea         rcx,[rbp-10h]
  00000001421AF109: mov         rdx,qword ptr [rbp+428h]
  00000001421AF110: call        _ZN12country_core9resources5roofs10roof_shape4Roof11gable_sides17h7e1ab50eb9948917E
  00000001421AF115: lea         rcx,[rbp+40h]
  00000001421AF119: lea         rdx,[rbp+0A8h]
  00000001421AF120: call        _ZN5utils8geometry9rectangle11Rectangle2d9as_curve217h349ec350bc0e155bE
  00000001421AF125: lea         rcx,[rbp+8]
  00000001421AF129: lea         rdx,[rbp+148h]
  00000001421AF130: call        _ZN5utils8geometry9rectangle11Rectangle2d9as_curve217h349ec350bc0e155bE
  00000001421AF135: movaps      xmm0,xmmword ptr [rbp+110h]
  00000001421AF13C: movaps      xmmword ptr [rbp+3F0h],xmm0
  00000001421AF143: mov         rax,qword ptr [rbp+428h]
  00000001421AF14A: mov         eax,dword ptr [rax]
  00000001421AF14C: mov         dword ptr [rbp+404h],eax
  00000001421AF152: xor         esi,esi
  00000001421AF154: lea         rdi,[rbp+338h]
  00000001421AF15B: lea         r13,[rbp+430h]
  00000001421AF162: lea         rbx,[rbp+0A8h]
  00000001421AF169: lea         r14,[rbp+270h]
  00000001421AF170: mov         dword ptr [rbp+474h],0
  00000001421AF17A: movaps      xmmword ptr [rbp+3C0h],xmm8
  00000001421AF182: jmp         00000001421AF1AC
  00000001421AF184: nop         word ptr cs:[rax+rax]
  00000001421AF190: mov         rsi,qword ptr [rbp+410h]
  00000001421AF197: lea         rdi,[rbp+338h]
  00000001421AF19E: lea         rbx,[rbp+0A8h]
  00000001421AF1A5: lea         r14,[rbp+270h]
  00000001421AF1AC: cmp         rsi,4
  00000001421AF1B0: jae         00000001421AFBFB
  00000001421AF1B6: xorps       xmm13,xmm13
  00000001421AF1BA: mov         rcx,qword ptr [rbp+428h]
  00000001421AF1C1: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  00000001421AF1C6: movaps      xmm11,xmm0
  00000001421AF1CA: mov         rcx,rdi
  00000001421AF1CD: lea         rdx,[rbp+148h]
  00000001421AF1D4: call        _ZN5utils8geometry9rectangle11Rectangle2d10as_points217h4b26c106db0a5c9aE
  00000001421AF1D9: movsd       xmm7,mmword ptr [rbp+rsi*8+338h]
  00000001421AF1E2: mov         rax,qword ptr [rbp+3E8h]
  00000001421AF1E9: movsd       xmm6,mmword ptr [rax+8]
  00000001421AF1EE: mov         rcx,r13
  00000001421AF1F1: mov         rdx,rbx
  00000001421AF1F4: call        _ZN5utils8geometry9rectangle11Rectangle2d10as_points217h4b26c106db0a5c9aE
  00000001421AF1F9: unpcklps    xmm7,xmm7
  00000001421AF1FC: movaps      xmm1,xmmword ptr [rbp+3F0h]
  00000001421AF203: mulps       xmm7,xmm1
  00000001421AF206: movaps      xmm0,xmm7
  00000001421AF209: unpckhpd    xmm0,xmm7
  00000001421AF20D: addps       xmm0,xmm7
  00000001421AF210: addps       xmm6,xmm0
  00000001421AF213: movsd       xmm0,mmword ptr [rbp+rsi*8+430h]
  00000001421AF21C: unpcklps    xmm0,xmm0
  00000001421AF21F: mulps       xmm0,xmm1
  00000001421AF222: movaps      xmm1,xmm0
  00000001421AF225: unpckhpd    xmm1,xmm0
  00000001421AF229: addps       xmm1,xmm0
  00000001421AF22C: mov         rax,qword ptr [rbp+3E8h]
  00000001421AF233: movsd       xmm0,mmword ptr [rax+8]
  00000001421AF238: addps       xmm0,xmm1
  00000001421AF23B: subps       xmm6,xmm0
  00000001421AF23E: movlps      qword ptr [rbp+270h],xmm6
  00000001421AF245: mov         rcx,r13
  00000001421AF248: mov         rdx,r14
  00000001421AF24B: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421AF250: movss       xmm9,dword ptr [rbp+430h]
  00000001421AF259: movsd       xmm7,mmword ptr [rbp+434h]
  00000001421AF261: mov         rcx,r13
  00000001421AF264: mov         rdx,r14
  00000001421AF267: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421AF26C: movsd       xmm6,mmword ptr [rbp+434h]
  00000001421AF274: movss       xmm14,dword ptr [rbp+430h]
  00000001421AF27D: mov         rcx,r13
  00000001421AF280: mov         rdx,rbx
  00000001421AF283: call        _ZN5utils8geometry9rectangle11Rectangle2d10as_points217h4b26c106db0a5c9aE
  00000001421AF288: movsd       xmm0,mmword ptr [rbp+rsi*8+430h]
  00000001421AF291: unpcklps    xmm0,xmm0
  00000001421AF294: mulps       xmm0,xmmword ptr [rbp+3F0h]
  00000001421AF29B: movaps      xmm1,xmm0
  00000001421AF29E: unpckhpd    xmm1,xmm0
  00000001421AF2A2: addps       xmm1,xmm0
  00000001421AF2A5: mov         rax,qword ptr [rbp+3E8h]
  00000001421AF2AC: movsd       xmm0,mmword ptr [rax+8]
  00000001421AF2B1: addps       xmm0,xmm1
  00000001421AF2B4: movlps      qword ptr [rbp+2B8h],xmm0
  00000001421AF2BB: mov         rcx,rdi
  00000001421AF2BE: lea         rdx,[rbp+2B8h]
  00000001421AF2C5: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421AF2CA: movaps      xmm10,xmm11
  00000001421AF2CE: mulss       xmm10,xmm13
  00000001421AF2D3: movsd       xmm8,mmword ptr [rbp+33Ch]
  00000001421AF2DC: movss       xmm15,dword ptr [rbp+338h]
  00000001421AF2E5: movsd       xmm0,mmword ptr [rbp+270h]
  00000001421AF2ED: mulps       xmm0,xmm0
  00000001421AF2F0: movshdup    xmm1,xmm0
  00000001421AF2F4: addss       xmm1,xmm0
  00000001421AF2F8: sqrtss      xmm3,xmm1
  00000001421AF2FC: movaps      xmm0,xmm10
  00000001421AF300: mulss       xmm0,xmm10
  00000001421AF305: movaps      xmm1,xmm11
  00000001421AF309: mulss       xmm1,xmm11
  00000001421AF30E: addss       xmm1,xmm0
  00000001421AF312: addss       xmm1,xmm0
  00000001421AF316: xorps       xmm0,xmm0
  00000001421AF319: sqrtss      xmm0,xmm1
  00000001421AF31D: movss       dword ptr [rsp+20h],xmm0
  00000001421AF323: xorps       xmm2,xmm2
  00000001421AF326: mov         rcx,r13
  00000001421AF329: lea         rdx,[rbp+190h]
  00000001421AF330: call        _ZN12country_core9resources5roofs10roof_shape16profile_curve_ws17h83456c52e1995788E
  00000001421AF335: movss       xmm12,dword ptr [rbp+460h]
  00000001421AF33E: mov         rdx,qword ptr [rbp+430h]
  00000001421AF345: test        rdx,rdx
  00000001421AF348: je          00000001421AF360
  00000001421AF34A: mov         rcx,qword ptr [rbp+438h]
  00000001421AF351: shl         rdx,3
  00000001421AF355: mov         r8d,4
  00000001421AF35B: call        __rust_dealloc
  00000001421AF360: mov         rdx,qword ptr [rbp+448h]
  00000001421AF367: test        rdx,rdx
  00000001421AF36A: je          00000001421AF382
  00000001421AF36C: mov         rcx,qword ptr [rbp+450h]
  00000001421AF373: shl         rdx,2
  00000001421AF377: mov         r8d,4
  00000001421AF37D: call        __rust_dealloc
  00000001421AF382: movaps      xmm0,xmm12
  00000001421AF386: divss       xmm0,dword ptr [__real@3f200000]
  00000001421AF38E: call        roundf
  00000001421AF393: maxss       xmm0,dword ptr [__real@40000000]
  00000001421AF39B: cvttss2si   rax,xmm0
  00000001421AF3A0: mov         rcx,rax
  00000001421AF3A3: sar         rcx,3Fh
  00000001421AF3A7: movaps      xmm1,xmm0
  00000001421AF3AA: subss       xmm1,dword ptr [__real@5f000000]
  00000001421AF3B2: cvttss2si   r8,xmm1
  00000001421AF3B7: and         r8,rcx
  00000001421AF3BA: or          r8,rax
  00000001421AF3BD: ucomiss     xmm0,xmm13
  00000001421AF3C1: mov         edx,0
  00000001421AF3C6: cmovae      rdx,r8
  00000001421AF3CA: ucomiss     xmm0,dword ptr [__real@5f7fffff]
  00000001421AF3D1: mov         rax,0FFFFFFFFFFFFFFFFh
  00000001421AF3D8: cmova       rdx,rax
  00000001421AF3DC: movss       xmm2,dword ptr [__real@3e000000]
  00000001421AF3E4: divss       xmm2,xmm12
  00000001421AF3E9: lea         rax,[142FF81F8h]
  00000001421AF3F0: mov         qword ptr [rsp+20h],rax
  00000001421AF3F5: mov         rcx,rdi
  00000001421AF3F8: lea         r9,[rbp+3A8h]
  00000001421AF3FF: call        _ZN5utils13random_splits17h445a73628407a507E
  00000001421AF404: inc         rsi
  00000001421AF407: mov         qword ptr [rbp+410h],rsi
  00000001421AF40E: movaps      xmm4,xmm10
  00000001421AF412: mulss       xmm4,xmm9
  00000001421AF417: movaps      xmm0,xmm11
  00000001421AF41B: mulss       xmm0,xmm9
  00000001421AF420: movaps      xmm5,xmm10
  00000001421AF424: unpcklps    xmm5,xmm11
  00000001421AF428: movaps      xmm1,xmm5
  00000001421AF42B: mulps       xmm1,xmm7
  00000001421AF42E: movshdup    xmm2,xmm7
  00000001421AF432: mulss       xmm2,xmm10
  00000001421AF437: subss       xmm4,xmm2
  00000001421AF43B: movsldup    xmm2,xmm1
  00000001421AF43F: movss       xmm2,xmm0
  00000001421AF443: subps       xmm1,xmm2
  00000001421AF446: movaps      xmm0,xmm4
  00000001421AF449: mulss       xmm0,xmm4
  00000001421AF44D: movaps      xmm2,xmm1
  00000001421AF450: mulps       xmm2,xmm1
  00000001421AF453: movshdup    xmm3,xmm2
  00000001421AF457: addss       xmm3,xmm0
  00000001421AF45B: addss       xmm3,xmm2
  00000001421AF45F: xorps       xmm0,xmm0
  00000001421AF462: sqrtss      xmm0,xmm3
  00000001421AF466: movss       xmm9,dword ptr [__real@3f800000]
  00000001421AF46F: movaps      xmm2,xmm9
  00000001421AF473: divss       xmm2,xmm0
  00000001421AF477: mulss       xmm4,xmm2
  00000001421AF47B: movsldup    xmm2,xmm2
  00000001421AF47F: mulps       xmm2,xmm1
  00000001421AF482: xorps       xmm0,xmm0
  00000001421AF485: movlhps     xmm0,xmm2
  00000001421AF488: movaps      xmm1,xmm4
  00000001421AF48B: shufps      xmm1,xmm2,0D4h
  00000001421AF48F: shufps      xmm1,xmm0,22h
  00000001421AF493: xorps       xmm0,xmm0
  00000001421AF496: movss       xmm0,xmm10
  00000001421AF49B: movlhps     xmm5,xmm0
  00000001421AF49E: movaps      xmmword ptr [rbp+250h],xmm5
  00000001421AF4A5: shufps      xmm14,xmm6,4Ch
  00000001421AF4AA: shufps      xmm14,xmm14,78h
  00000001421AF4AF: movaps      xmmword ptr [rbp+240h],xmm14
  00000001421AF4B7: shufpd      xmm15,xmmword ptr [__xmm@3f800000000000000000000000000000],2
  00000001421AF4C1: shufps      xmm15,xmm8,4Ch
  00000001421AF4C6: shufps      xmm15,xmm15,78h
  00000001421AF4CB: movaps      xmmword ptr [rbp+2A0h],xmm15
  00000001421AF4D3: movaps      xmm15,xmm9
  00000001421AF4D7: mov         r15,qword ptr [rbp+340h]
  00000001421AF4DE: mov         rsi,qword ptr [rbp+348h]
  00000001421AF4E5: lea         r12,[rsi-1]
  00000001421AF4E9: mulps       xmm1,xmmword ptr [__xmm@00000000000000000000000000000000]
  00000001421AF4F0: movaps      xmmword ptr [rbp+120h],xmm1
  00000001421AF4F7: movaps      xmmword ptr [rbp+260h],xmm2
  00000001421AF4FE: movsldup    xmm0,xmm2
  00000001421AF502: movaps      xmmword ptr [rbp+300h],xmm4
  00000001421AF509: movss       xmm0,xmm4
  00000001421AF50D: movaps      xmmword ptr [rbp+130h],xmm0
  00000001421AF514: xor         ebx,ebx
  00000001421AF516: xorps       xmm10,xmm10
  00000001421AF51A: cmp         rbx,r12
  00000001421AF51D: jae         00000001421AFBD0
  00000001421AF523: nop         word ptr cs:[rax+rax]
  00000001421AF530: cmp         rsi,rbx
  00000001421AF533: jbe         00000001421B2217
  00000001421AF539: movss       xmm11,dword ptr [r15+rbx*4]
  00000001421AF53F: movss       dword ptr [rbp+280h],xmm11
  00000001421AF548: ucomiss     xmm11,xmm10
  00000001421AF54C: jb          00000001421AFC7B
  00000001421AF552: ucomiss     xmm15,xmm11
  00000001421AF556: jb          00000001421AFC7B
  00000001421AF55C: ucomiss     xmm11,xmm11
  00000001421AF560: jp          00000001421AFCEC
  00000001421AF566: inc         rbx
  00000001421AF569: cmp         rbx,rsi
  00000001421AF56C: jae         00000001421B222E
  00000001421AF572: movss       xmm12,dword ptr [r15+rbx*4]
  00000001421AF578: movss       dword ptr [rbp+280h],xmm12
  00000001421AF581: ucomiss     xmm12,xmm10
  00000001421AF585: jb          00000001421AFD09
  00000001421AF58B: ucomiss     xmm15,xmm12
  00000001421AF58F: jb          00000001421AFD09
  00000001421AF595: ucomiss     xmm12,xmm12
  00000001421AF599: jp          00000001421AFD7A
  00000001421AF59F: lea         rcx,[rbp+190h]
  00000001421AF5A6: movaps      xmm1,xmm11
  00000001421AF5AA: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001421AF5AF: mov         rdx,qword ptr [rbp+1A0h]
  00000001421AF5B6: mov         ecx,eax
  00000001421AF5B8: cmp         rdx,rcx
  00000001421AF5BB: jbe         00000001421B2245
  00000001421AF5C1: lea         rax,[rcx+1]
  00000001421AF5C5: cmp         rax,rdx
  00000001421AF5C8: jae         00000001421B2267
  00000001421AF5CE: mov         rax,qword ptr [rbp+198h]
  00000001421AF5D5: movaps      xmm6,xmm15
  00000001421AF5D9: subss       xmm6,xmm0
  00000001421AF5DD: movss       xmm14,dword ptr [rax+rcx*8]
  00000001421AF5E3: mulss       xmm14,xmm6
  00000001421AF5E8: mulss       xmm6,dword ptr [rax+rcx*8+4]
  00000001421AF5EE: movss       xmm7,dword ptr [rax+rcx*8+8]
  00000001421AF5F4: mulss       xmm7,xmm0
  00000001421AF5F8: movss       xmm8,dword ptr [rax+rcx*8+0Ch]
  00000001421AF5FF: mulss       xmm8,xmm0
  00000001421AF604: lea         rcx,[rbp+190h]
  00000001421AF60B: movaps      xmm1,xmm12
  00000001421AF60F: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001421AF614: mov         rdx,qword ptr [rbp+1A0h]
  00000001421AF61B: mov         ecx,eax
  00000001421AF61D: cmp         rdx,rcx
  00000001421AF620: jbe         00000001421B2256
  00000001421AF626: lea         rax,[rcx+1]
  00000001421AF62A: cmp         rax,rdx
  00000001421AF62D: jae         00000001421B227B
  00000001421AF633: movss       dword ptr [rbp+390h],xmm11
  00000001421AF63C: addss       xmm14,xmm7
  00000001421AF641: addss       xmm6,xmm8
  00000001421AF646: mov         rax,qword ptr [rbp+198h]
  00000001421AF64D: movaps      xmm2,xmm15
  00000001421AF651: subss       xmm2,xmm0
  00000001421AF655: movss       xmm3,dword ptr [rax+rcx*8]
  00000001421AF65A: mulss       xmm3,xmm2
  00000001421AF65E: mulss       xmm2,dword ptr [rax+rcx*8+4]
  00000001421AF664: movss       xmm1,dword ptr [rax+rcx*8+8]
  00000001421AF66A: mulss       xmm1,xmm0
  00000001421AF66E: movss       xmm4,dword ptr [rax+rcx*8+0Ch]
  00000001421AF674: mulss       xmm4,xmm0
  00000001421AF678: addss       xmm1,xmm3
  00000001421AF67C: addss       xmm4,xmm2
  00000001421AF680: shufps      xmm6,xmm6,0
  00000001421AF684: movaps      xmm3,xmmword ptr [rbp+250h]
  00000001421AF68B: mulps       xmm6,xmm3
  00000001421AF68E: movaps      xmm5,xmmword ptr [rbp+120h]
  00000001421AF695: addps       xmm6,xmm5
  00000001421AF698: shufps      xmm14,xmm14,0
  00000001421AF69D: movaps      xmm0,xmmword ptr [rbp+240h]
  00000001421AF6A4: mulps       xmm14,xmm0
  00000001421AF6A8: addps       xmm14,xmm6
  00000001421AF6AC: movaps      xmm2,xmmword ptr [rbp+2A0h]
  00000001421AF6B3: addps       xmm14,xmm2
  00000001421AF6B7: shufps      xmm4,xmm4,0
  00000001421AF6BB: mulps       xmm4,xmm3
  00000001421AF6BE: addps       xmm4,xmm5
  00000001421AF6C1: shufps      xmm1,xmm1,0
  00000001421AF6C5: mulps       xmm1,xmm0
  00000001421AF6C8: addps       xmm1,xmm4
  00000001421AF6CB: addps       xmm1,xmm2
  00000001421AF6CE: movaps      xmm0,xmm14
  00000001421AF6D2: subps       xmm0,xmm1
  00000001421AF6D5: mulps       xmm0,xmm0
  00000001421AF6D8: movshdup    xmm2,xmm0
  00000001421AF6DC: addss       xmm2,xmm0
  00000001421AF6E0: movhlps     xmm0,xmm0
  00000001421AF6E3: addss       xmm0,xmm2
  00000001421AF6E7: sqrtss      xmm0,xmm0
  00000001421AF6EB: addss       xmm0,dword ptr [__real@3d4ccccd]
  00000001421AF6F3: movaps      xmmword ptr [rbp+2F0h],xmm0
  00000001421AF6FA: subps       xmm1,xmm14
  00000001421AF6FE: movaps      xmm4,xmm1
  00000001421AF701: unpckhpd    xmm4,xmm1
  00000001421AF705: movaps      xmm0,xmm4
  00000001421AF708: mulss       xmm0,xmm4
  00000001421AF70C: movaps      xmm2,xmm1
  00000001421AF70F: mulps       xmm2,xmm1
  00000001421AF712: movshdup    xmm3,xmm2
  00000001421AF716: addss       xmm3,xmm2
  00000001421AF71A: addss       xmm3,xmm0
  00000001421AF71E: xorps       xmm0,xmm0
  00000001421AF721: sqrtss      xmm0,xmm3
  00000001421AF725: movaps      xmm13,xmm15
  00000001421AF729: divss       xmm13,xmm0
  00000001421AF72E: movsldup    xmm3,xmm13
  00000001421AF733: mulps       xmm3,xmm1
  00000001421AF736: movaps      xmm8,xmmword ptr [rbp+300h]
  00000001421AF73E: mulss       xmm8,xmm3
  00000001421AF743: movshdup    xmm15,xmm3
  00000001421AF748: movaps      xmm7,xmmword ptr [rbp+260h]
  00000001421AF74F: movaps      xmm0,xmm7
  00000001421AF752: mulps       xmm0,xmm3
  00000001421AF755: movaps      xmm2,xmm3
  00000001421AF758: movaps      xmm1,xmm3
  00000001421AF75B: mulss       xmm13,xmm4
  00000001421AF760: movaps      xmm12,xmm13
  00000001421AF764: shufps      xmm12,xmm3,0D4h
  00000001421AF769: shufps      xmm12,xmm3,0E2h
  00000001421AF76E: movaps      xmm4,xmm3
  00000001421AF771: movlhps     xmm4,xmm13
  00000001421AF775: shufps      xmm4,xmm3,0E2h
  00000001421AF779: movaps      xmm6,xmm3
  00000001421AF77C: movaps      xmm5,xmm3
  00000001421AF77F: shufps      xmm5,xmm3,0E1h
  00000001421AF783: movshdup    xmm0,xmm0
  00000001421AF787: subss       xmm8,xmm0
  00000001421AF78C: movaps      xmm0,xmm15
  00000001421AF790: mulss       xmm0,xmm8
  00000001421AF795: mulss       xmm2,xmm8
  00000001421AF79A: mulss       xmm1,dword ptr [__real@b3bbbd2e]
  00000001421AF7A2: mulps       xmm12,xmm7
  00000001421AF7A6: mulps       xmm4,xmmword ptr [rbp+130h]
  00000001421AF7AD: subps       xmm12,xmm4
  00000001421AF7B1: movshdup    xmm7,xmm12
  00000001421AF7B6: movaps      xmm4,xmm13
  00000001421AF7BA: mulss       xmm4,xmm7
  00000001421AF7BE: subss       xmm0,xmm4
  00000001421AF7C2: movaps      xmm10,xmm13
  00000001421AF7C6: mulss       xmm10,xmm12
  00000001421AF7CB: subss       xmm10,xmm2
  00000001421AF7D0: mulss       xmm6,xmm7
  00000001421AF7D4: movaps      xmm2,xmm15
  00000001421AF7D8: mulss       xmm2,xmm12
  00000001421AF7DD: subss       xmm6,xmm2
  00000001421AF7E1: subss       xmm1,xmm0
  00000001421AF7E5: movsldup    xmm9,xmm12
  00000001421AF7EA: movss       xmm9,xmm8
  00000001421AF7EF: mulps       xmm9,xmmword ptr [__xmm@00000000000000000000000000000000]
  00000001421AF7F7: movshdup    xmm4,xmm9
  00000001421AF7FC: addss       xmm4,xmm1
  00000001421AF800: movaps      xmm1,xmm13
  00000001421AF804: unpcklps    xmm1,xmm0
  00000001421AF807: mulps       xmm1,xmmword ptr [__xmm@000000000000000033bbbd2eb3bbbd2e]
  00000001421AF80E: movss       xmm5,xmm6
  00000001421AF812: subps       xmm1,xmm5
  00000001421AF815: movaps      xmm5,xmm10
  00000001421AF819: movss       xmm11,dword ptr [__real@33bbbd2e]
  00000001421AF822: mulss       xmm5,xmm11
  00000001421AF827: movaps      xmm2,xmm6
  00000001421AF82A: mulss       xmm2,xmm11
  00000001421AF82F: subss       xmm5,xmm15
  00000001421AF834: subss       xmm2,xmm13
  00000001421AF839: addps       xmm1,xmm9
  00000001421AF83D: addss       xmm2,xmm9
  00000001421AF842: movsldup    xmm9,xmm10
  00000001421AF847: movaps      xmmword ptr [rbp+380h],xmm9
  00000001421AF84F: xorps       xmm11,xmm11
  00000001421AF853: mulss       xmm10,xmm11
  00000001421AF858: mulss       xmm6,xmm11
  00000001421AF85D: mulss       xmm15,xmm11
  00000001421AF862: addss       xmm15,xmm10
  00000001421AF867: movaps      xmm10,xmm13
  00000001421AF86B: mulss       xmm10,xmm11
  00000001421AF870: addss       xmm10,xmm6
  00000001421AF875: movaps      xmm11,xmmword ptr [rbp+2F0h]
  00000001421AF87D: movsldup    xmm9,xmm11
  00000001421AF882: mulps       xmm9,xmm3
  00000001421AF886: xorps       xmm6,xmm6
  00000001421AF889: mulss       xmm0,xmm6
  00000001421AF88D: mulps       xmm3,xmmword ptr [__xmm@0000000000000000b3bbbd2e00000000]
  00000001421AF894: addps       xmm0,xmm3
  00000001421AF897: subps       xmm3,xmmword ptr [rbp+380h]
  00000001421AF89E: shufps      xmm3,xmm0,1
  00000001421AF8A2: shufps      xmm3,xmm0,0E2h
  00000001421AF8A6: addss       xmm15,xmm7
  00000001421AF8AB: mulss       xmm7,xmm6
  00000001421AF8AF: addss       xmm5,xmm7
  00000001421AF8B3: movlhps     xmm7,xmm12
  00000001421AF8B7: shufps      xmm7,xmm12,0E2h
  00000001421AF8BC: addps       xmm7,xmm3
  00000001421AF8BF: xorps       xmm3,xmm3
  00000001421AF8C2: addss       xmm10,xmm8
  00000001421AF8C7: mulss       xmm13,xmm11
  00000001421AF8CC: mulps       xmm9,xmmword ptr [__xmm@00000000000000003e99999a3e99999a]
  00000001421AF8D4: mulss       xmm13,dword ptr [__real@3e99999a]
  00000001421AF8DD: mulps       xmm12,xmmword ptr [__xmm@0000000000000000bd4ccccdbd4ccccd]
  00000001421AF8E5: mulss       xmm8,dword ptr [__real@bd4ccccd]
  00000001421AF8EE: ucomiss     xmm3,xmm10
  00000001421AF8F2: jae         00000001421AF990
  00000001421AF8F8: addss       xmm4,xmm5
  00000001421AF8FC: movss       xmm0,dword ptr [__real@3f800000]
  00000001421AF904: addss       xmm10,xmm0
  00000001421AF909: ucomiss     xmm3,xmm4
  00000001421AF90C: movaps      xmm6,xmm0
  00000001421AF90F: movss       xmm11,dword ptr [rbp+390h]
  00000001421AF918: jae         00000001421AFA20
  00000001421AF91E: movlhps     xmm2,xmm7
  00000001421AF921: movaps      xmm0,xmm2
  00000001421AF924: shufps      xmm0,xmm7,0D8h
  00000001421AF928: shufps      xmm4,xmm7,0D4h
  00000001421AF92C: shufps      xmm2,xmm4,28h
  00000001421AF930: movlhps     xmm15,xmm1
  00000001421AF934: movaps      xmm3,xmm15
  00000001421AF938: shufps      xmm3,xmm1,0D8h
  00000001421AF93C: subps       xmm0,xmm3
  00000001421AF93F: shufps      xmm10,xmm1,0D4h
  00000001421AF944: shufps      xmm15,xmm10,28h
  00000001421AF949: addps       xmm15,xmm2
  00000001421AF94D: movaps      xmm1,xmm15
  00000001421AF951: shufps      xmm1,xmm0,23h
  00000001421AF955: shufps      xmm0,xmm1,24h
  00000001421AF959: shufps      xmm15,xmm15,0FFh
  00000001421AF95E: xorps       xmm2,xmm2
  00000001421AF961: sqrtss      xmm2,xmm15
  00000001421AF966: movss       xmm1,dword ptr [__real@3f000000]
  00000001421AF96E: divss       xmm1,xmm2
  00000001421AF972: shufps      xmm1,xmm1,0
  00000001421AF976: mulps       xmm1,xmm0
  00000001421AF979: xorps       xmm10,xmm10
  00000001421AF97D: jmp         00000001421AFACC
  00000001421AF982: nop         word ptr cs:[rax+rax]
  00000001421AF990: subss       xmm5,xmm4
  00000001421AF994: movss       xmm4,dword ptr [__real@3f800000]
  00000001421AF99C: movaps      xmm0,xmm4
  00000001421AF99F: subss       xmm0,xmm10
  00000001421AF9A4: ucomiss     xmm3,xmm5
  00000001421AF9A7: movaps      xmm6,xmm4
  00000001421AF9AA: xorps       xmm10,xmm10
  00000001421AF9AE: movss       xmm11,dword ptr [rbp+390h]
  00000001421AF9B7: movaps      xmm3,xmm7
  00000001421AF9BA: jae         00000001421AFA80
  00000001421AF9C0: shufps      xmm3,xmm7,25h
  00000001421AF9C4: shufps      xmm5,xmm7,0D4h
  00000001421AF9C8: movlhps     xmm15,xmm7
  00000001421AF9CC: shufps      xmm5,xmm15,82h
  00000001421AF9D1: movaps      xmm4,xmm1
  00000001421AF9D4: shufps      xmm4,xmm1,25h
  00000001421AF9D8: subps       xmm3,xmm4
  00000001421AF9DB: shufps      xmm0,xmm1,0D4h
  00000001421AF9DF: movlhps     xmm2,xmm1
  00000001421AF9E2: shufps      xmm0,xmm2,82h
  00000001421AF9E6: addps       xmm0,xmm5
  00000001421AF9E9: shufps      xmm3,xmm0,23h
  00000001421AF9ED: movshdup    xmm1,xmm0
  00000001421AF9F1: shufps      xmm0,xmm3,24h
  00000001421AF9F5: xorps       xmm2,xmm2
  00000001421AF9F8: sqrtss      xmm2,xmm1
  00000001421AF9FC: movss       xmm1,dword ptr [__real@3f000000]
  00000001421AFA04: divss       xmm1,xmm2
  00000001421AFA08: shufps      xmm1,xmm1,0
  00000001421AFA0C: mulps       xmm1,xmm0
  00000001421AFA0F: jmp         00000001421AFACC
  00000001421AFA14: nop         word ptr cs:[rax+rax]
  00000001421AFA20: movlhps     xmm2,xmm1
  00000001421AFA23: movaps      xmm0,xmm2
  00000001421AFA26: shufps      xmm0,xmm1,0E2h
  00000001421AFA2A: movaps      xmm3,xmm7
  00000001421AFA2D: shufps      xmm3,xmm10,0C5h
  00000001421AFA32: shufps      xmm2,xmm3,22h
  00000001421AFA36: movlhps     xmm15,xmm7
  00000001421AFA3A: movaps      xmm3,xmm15
  00000001421AFA3E: shufps      xmm3,xmm7,0E2h
  00000001421AFA42: addps       xmm3,xmm0
  00000001421AFA45: shufps      xmm1,xmm4,0C5h
  00000001421AFA49: shufps      xmm15,xmm1,22h
  00000001421AFA4E: subps       xmm2,xmm15
  00000001421AFA52: shufps      xmm3,xmm2,0E4h
  00000001421AFA56: movhlps     xmm2,xmm2
  00000001421AFA59: xorps       xmm0,xmm0
  00000001421AFA5C: sqrtss      xmm0,xmm2
  00000001421AFA60: movss       xmm1,dword ptr [__real@3f000000]
  00000001421AFA68: divss       xmm1,xmm0
  00000001421AFA6C: shufps      xmm1,xmm1,0
  00000001421AFA70: mulps       xmm1,xmm3
  00000001421AFA73: xorps       xmm10,xmm10
  00000001421AFA77: jmp         00000001421AFACC
  00000001421AFA79: nop         dword ptr [rax]
  00000001421AFA80: movss       xmm3,xmm0
  00000001421AFA84: movaps      xmm0,xmm3
  00000001421AFA87: movlhps     xmm0,xmm1
  00000001421AFA8A: movlhps     xmm2,xmm1
  00000001421AFA8D: shufps      xmm3,xmm2,24h
  00000001421AFA91: movss       xmm1,xmm5
  00000001421AFA95: movaps      xmm2,xmm1
  00000001421AFA98: movlhps     xmm2,xmm7
  00000001421AFA9B: addps       xmm2,xmm0
  00000001421AFA9E: movlhps     xmm15,xmm7
  00000001421AFAA2: shufps      xmm1,xmm15,24h
  00000001421AFAA7: subps       xmm3,xmm1
  00000001421AFAAA: xorps       xmm0,xmm0
  00000001421AFAAD: sqrtss      xmm0,xmm3
  00000001421AFAB1: shufps      xmm3,xmm2,9Ch
  00000001421AFAB5: shufps      xmm3,xmm3,78h
  00000001421AFAB9: movss       xmm1,dword ptr [__real@3f000000]
  00000001421AFAC1: divss       xmm1,xmm0
  00000001421AFAC5: shufps      xmm1,xmm1,0
  00000001421AFAC9: mulps       xmm1,xmm3
  00000001421AFACC: movaps      xmmword ptr [rbp-50h],xmm1
  00000001421AFAD0: lea         rcx,[rbp-50h]
  00000001421AFAD4: call        _ZN69_$LT$glam..f32..sse2..quat..Quat$u20$as$u20$utils..halver..Halver$GT$6to_f1617h7d373b4258af3426E
  00000001421AFAD9: mov         rdi,rax
  00000001421AFADC: movaps      xmm15,xmm6
  00000001421AFAE0: mov         dword ptr [rbp+430h],3E800000h
  00000001421AFAEA: movaps      xmm0,xmmword ptr [rbp+2F0h]
  00000001421AFAF1: movss       dword ptr [rbp+434h],xmm0
  00000001421AFAF9: mov         dword ptr [rbp+438h],3E3851ECh
  00000001421AFB03: mov         rcx,r13
  00000001421AFB06: call        _ZN66_$LT$$u5b$f32$u3b$$u20$3$u5d$$u20$as$u20$utils..halver..Halver$GT$6to_f1617ha8b503566bb8cdf3E
  00000001421AFB0B: mov         r14,rax
  00000001421AFB0E: movaps      xmm0,xmm11
  00000001421AFB12: call        _ZN45_$LT$f32$u20$as$u20$utils..halver..Halver$GT$6to_f1617h628d79fa31310e7aE
  00000001421AFB17: movaps      xmm0,xmm14
  00000001421AFB1B: unpckhpd    xmm0,xmm14
  00000001421AFB20: addps       xmm9,xmm14
  00000001421AFB24: addps       xmm9,xmm12
  00000001421AFB28: addss       xmm0,xmm13
  00000001421AFB2D: addss       xmm0,xmm8
  00000001421AFB32: movlps      qword ptr [rbp+430h],xmm9
  00000001421AFB3A: movss       dword ptr [rbp+438h],xmm0
  00000001421AFB42: mov         dword ptr [rbp+43Ch],0
  00000001421AFB4C: mov         rcx,7BFF00003C000000h
  00000001421AFB56: mov         qword ptr [rbp+440h],rcx
  00000001421AFB5D: mov         dword ptr [rbp+448h],r14d
  00000001421AFB64: shr         r14,20h
  00000001421AFB68: mov         word ptr [rbp+44Ch],r14w
  00000001421AFB70: mov         word ptr [rbp+44Eh],ax
  00000001421AFB77: mov         qword ptr [rbp+450h],rdi
  00000001421AFB7E: mov         eax,dword ptr [rbp+404h]
  00000001421AFB84: mov         dword ptr [rbp+458h],eax
  00000001421AFB8A: mov         qword ptr [rbp+45Ch],0
  00000001421AFB95: mov         eax,dword ptr [rbp+474h]
  00000001421AFB9B: mov         dword ptr [rbp+464h],eax
  00000001421AFBA1: mov         rax,200000000h
  00000001421AFBAB: mov         qword ptr [rbp+468h],rax
  00000001421AFBB2: mov         rcx,qword ptr [rbp+5A0h]
  00000001421AFBB9: mov         rdx,r13
  00000001421AFBBC: call        00000001421B2CA0
  00000001421AFBC1: inc         dword ptr [rbp+474h]
  00000001421AFBC7: cmp         rbx,r12
  00000001421AFBCA: jb          00000001421AF530
  00000001421AFBD0: mov         rdx,qword ptr [rbp+338h]
  00000001421AFBD7: test        rdx,rdx
  00000001421AFBDA: je          00000001421AF190
  00000001421AFBE0: mov         rcx,qword ptr [rbp+340h]
  00000001421AFBE7: shl         rdx,2
  00000001421AFBEB: mov         r8d,4
  00000001421AFBF1: call        __rust_dealloc
  00000001421AFBF6: jmp         00000001421AF190
  00000001421AFBFB: mov         rcx,qword ptr [rbp+428h]
  00000001421AFC02: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims4from17hcb00f4a21c308a29E
  00000001421AFC07: mov         qword ptr [rbp+1F0h],rax
  00000001421AFC0E: lea         rcx,[rbp+338h]
  00000001421AFC15: lea         rdx,[rbp+1F0h]
  00000001421AFC1C: mov         r8,qword ptr [rbp+428h]
  00000001421AFC23: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims14to_world_space17h36c23ee40b8867c8E
  00000001421AFC28: cmp         dword ptr [rbp+338h],1
  00000001421AFC2F: jne         00000001421B062D
  00000001421AFC35: movaps      xmm2,xmmword ptr [rbp+3C0h]
  00000001421AFC3C: movaps      xmm1,xmmword ptr [rbp+170h]
  00000001421AFC43: ucomiss     xmm2,xmm1
  00000001421AFC46: movsd       xmm0,mmword ptr [rbp+148h]
  00000001421AFC4E: movsd       xmm7,mmword ptr [rbp+150h]
  00000001421AFC56: jbe         00000001421AFD97
  00000001421AFC5C: movshdup    xmm1,xmm0
  00000001421AFC60: xorps       xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421AFC67: movsldup    xmm2,xmm2
  00000001421AFC6B: movsldup    xmm6,xmm0
  00000001421AFC6F: movss       xmm6,xmm1
  00000001421AFC73: mulps       xmm6,xmm2
  00000001421AFC76: jmp         00000001421AFD9E
  00000001421AFC7B: lea         rax,[rbp+280h]
  00000001421AFC82: mov         qword ptr [rbp+2B8h],rax
  00000001421AFC89: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  00000001421AFC90: mov         qword ptr [rbp+2C0h],rax
  00000001421AFC97: lea         rax,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.1.llvm.17463645933293978377]
  00000001421AFC9E: mov         qword ptr [rbp+430h],rax
  00000001421AFCA5: mov         qword ptr [rbp+438h],1
  00000001421AFCB0: mov         qword ptr [rbp+450h],0
  00000001421AFCBB: lea         rax,[rbp+2B8h]
  00000001421AFCC2: mov         qword ptr [rbp+440h],rax
  00000001421AFCC9: mov         qword ptr [rbp+448h],1
  00000001421AFCD4: lea         rdx,[142FF8228h]
  00000001421AFCDB: lea         rcx,[rbp+430h]
  00000001421AFCE2: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  00000001421AFCE7: jmp         00000001421B23A9
  00000001421AFCEC: lea         rcx,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.2.llvm.17463645933293978377]
  00000001421AFCF3: lea         r8,[142FF8228h]
  00000001421AFCFA: mov         edx,1Dh
  00000001421AFCFF: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001421AFD04: jmp         00000001421B23A9
  00000001421AFD09: lea         rax,[rbp+280h]
  00000001421AFD10: mov         qword ptr [rbp+2B8h],rax
  00000001421AFD17: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  00000001421AFD1E: mov         qword ptr [rbp+2C0h],rax
  00000001421AFD25: lea         rax,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.1.llvm.17463645933293978377]
  00000001421AFD2C: mov         qword ptr [rbp+430h],rax
  00000001421AFD33: mov         qword ptr [rbp+438h],1
  00000001421AFD3E: mov         qword ptr [rbp+450h],0
  00000001421AFD49: lea         rax,[rbp+2B8h]
  00000001421AFD50: mov         qword ptr [rbp+440h],rax
  00000001421AFD57: mov         qword ptr [rbp+448h],1
  00000001421AFD62: lea         rdx,[142FF8258h]
  00000001421AFD69: lea         rcx,[rbp+430h]
  00000001421AFD70: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  00000001421AFD75: jmp         00000001421B23A9
  00000001421AFD7A: lea         rcx,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.2.llvm.17463645933293978377]
  00000001421AFD81: lea         r8,[142FF8258h]
  00000001421AFD88: mov         edx,1Dh
  00000001421AFD8D: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001421AFD92: jmp         00000001421B23A9
  00000001421AFD97: movsldup    xmm6,xmm1
  00000001421AFD9B: mulps       xmm6,xmm0
  00000001421AFD9E: mulps       xmm6,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421AFDA5: movaps      xmm0,xmm7
  00000001421AFDA8: subps       xmm0,xmm6
  00000001421AFDAB: unpcklps    xmm0,xmm0
  00000001421AFDAE: movaps      xmm8,xmmword ptr [rbp+110h]
  00000001421AFDB6: mulps       xmm0,xmm8
  00000001421AFDBA: movaps      xmm1,xmm0
  00000001421AFDBD: unpckhpd    xmm1,xmm0
  00000001421AFDC1: addps       xmm1,xmm0
  00000001421AFDC4: mov         rax,qword ptr [rbp+3E8h]
  00000001421AFDCB: movsd       xmm0,mmword ptr [rax+8]
  00000001421AFDD0: addps       xmm0,xmm1
  00000001421AFDD3: movlps      qword ptr [rbp+2B8h],xmm0
  00000001421AFDDA: lea         rcx,[rbp+430h]
  00000001421AFDE1: lea         rdx,[rbp+2B8h]
  00000001421AFDE8: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421AFDED: mov         rcx,qword ptr [rbp+428h]
  00000001421AFDF4: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  00000001421AFDF9: movaps      xmm9,xmm0
  00000001421AFDFD: addps       xmm7,xmm6
  00000001421AFE00: unpcklps    xmm7,xmm7
  00000001421AFE03: xorps       xmm12,xmm12
  00000001421AFE07: mulss       xmm12,xmm0
  00000001421AFE0C: movsd       xmm11,mmword ptr [rbp+430h]
  00000001421AFE15: movss       xmm13,dword ptr [rbp+438h]
  00000001421AFE1E: addss       xmm13,xmm12
  00000001421AFE23: mulps       xmm8,xmm7
  00000001421AFE27: movaps      xmm0,xmm8
  00000001421AFE2B: unpckhpd    xmm0,xmm8
  00000001421AFE30: addps       xmm0,xmm8
  00000001421AFE34: mov         rax,qword ptr [rbp+3E8h]
  00000001421AFE3B: movsd       xmm1,mmword ptr [rax+8]
  00000001421AFE40: addps       xmm1,xmm0
  00000001421AFE43: movlps      qword ptr [rbp+2B8h],xmm1
  00000001421AFE4A: lea         rcx,[rbp+430h]
  00000001421AFE51: lea         rdx,[rbp+2B8h]
  00000001421AFE58: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421AFE5D: mov         rcx,qword ptr [rbp+428h]
  00000001421AFE64: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  00000001421AFE69: movaps      xmm10,xmm0
  00000001421AFE6D: xorps       xmm8,xmm8
  00000001421AFE71: mulss       xmm8,xmm0
  00000001421AFE76: movsd       xmm6,mmword ptr [rbp+430h]
  00000001421AFE7E: movss       xmm15,dword ptr [rbp+438h]
  00000001421AFE87: addss       xmm15,xmm8
  00000001421AFE8C: mov         rcx,qword ptr [rbp+428h]
  00000001421AFE93: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  00000001421AFE98: unpcklps    xmm12,xmm9
  00000001421AFE9C: addps       xmm12,xmm11
  00000001421AFEA0: unpcklps    xmm8,xmm10
  00000001421AFEA4: addps       xmm8,xmm6
  00000001421AFEA8: test        al,al
  00000001421AFEAA: je          00000001421AFF5E
  00000001421AFEB0: movaps      xmm1,xmm8
  00000001421AFEB4: subps       xmm1,xmm12
  00000001421AFEB8: movaps      xmm0,xmm15
  00000001421AFEBC: subss       xmm0,xmm13
  00000001421AFEC1: movaps      xmm2,xmm1
  00000001421AFEC4: mulps       xmm2,xmm1
  00000001421AFEC7: movshdup    xmm3,xmm2
  00000001421AFECB: addss       xmm3,xmm2
  00000001421AFECF: movaps      xmm2,xmm0
  00000001421AFED2: mulss       xmm2,xmm0
  00000001421AFED6: addss       xmm2,xmm3
  00000001421AFEDA: xorps       xmm3,xmm3
  00000001421AFEDD: sqrtss      xmm3,xmm2
  00000001421AFEE1: movss       xmm2,dword ptr [__real@3f800000]
  00000001421AFEE9: divss       xmm2,xmm3
  00000001421AFEED: mov         rax,qword ptr [rbp+428h]
  00000001421AFEF4: cmp         byte ptr [rax+54h],1
  00000001421AFEF8: jne         00000001421AFF23
  00000001421AFEFA: movsldup    xmm3,xmm2
  00000001421AFEFE: mulps       xmm1,xmm3
  00000001421AFF01: mulss       xmm0,xmm2
  00000001421AFF05: mulps       xmm1,xmmword ptr [__xmm@00000000000000003f0f5c293f0f5c29]
  00000001421AFF0C: mulss       xmm0,dword ptr [__real@3f0f5c29]
  00000001421AFF14: subps       xmm12,xmm1
  00000001421AFF18: subss       xmm13,xmm0
  00000001421AFF1D: addps       xmm8,xmm1
  00000001421AFF21: jmp         00000001421AFF59
  00000001421AFF23: mulss       xmm0,xmm2
  00000001421AFF27: mulss       xmm0,dword ptr [__real@3f0f5c29]
  00000001421AFF2F: movsldup    xmm2,xmm2
  00000001421AFF33: mulps       xmm2,xmm1
  00000001421AFF36: mulps       xmm2,xmmword ptr [__xmm@00000000000000003f0f5c293f0f5c29]
  00000001421AFF3D: mulps       xmm2,xmmword ptr [__xmm@0000000000000000bf000000bf000000]
  00000001421AFF44: mulss       xmm0,dword ptr [__real@bf000000]
  00000001421AFF4C: subps       xmm12,xmm2
  00000001421AFF50: subss       xmm13,xmm0
  00000001421AFF55: addps       xmm8,xmm2
  00000001421AFF59: addss       xmm15,xmm0
  00000001421AFF5E: movaps      xmm9,xmm8
  00000001421AFF62: subps       xmm9,xmm12
  00000001421AFF66: movaps      xmm10,xmm15
  00000001421AFF6A: subss       xmm10,xmm13
  00000001421AFF6F: movaps      xmm0,xmm9
  00000001421AFF73: mulps       xmm0,xmm9
  00000001421AFF77: movshdup    xmm1,xmm0
  00000001421AFF7B: addss       xmm1,xmm0
  00000001421AFF7F: movaps      xmm0,xmm10
  00000001421AFF83: mulss       xmm0,xmm10
  00000001421AFF88: addss       xmm0,xmm1
  00000001421AFF8C: xorps       xmm11,xmm11
  00000001421AFF90: sqrtss      xmm11,xmm0
  00000001421AFF95: movaps      xmm0,xmm11
  00000001421AFF99: divss       xmm0,dword ptr [__real@3f200000]
  00000001421AFFA1: call        roundf
  00000001421AFFA6: maxss       xmm0,dword ptr [__real@40000000]
  00000001421AFFAE: cvttss2si   rax,xmm0
  00000001421AFFB3: mov         rcx,rax
  00000001421AFFB6: sar         rcx,3Fh
  00000001421AFFBA: movaps      xmm1,xmm0
  00000001421AFFBD: subss       xmm1,dword ptr [__real@5f000000]
  00000001421AFFC5: cvttss2si   rdx,xmm1
  00000001421AFFCA: and         rdx,rcx
  00000001421AFFCD: or          rdx,rax
  00000001421AFFD0: xor         eax,eax
  00000001421AFFD2: xorps       xmm7,xmm7
  00000001421AFFD5: ucomiss     xmm0,xmm7
  00000001421AFFD8: cmovae      rax,rdx
  00000001421AFFDC: ucomiss     xmm0,dword ptr [__real@5f7fffff]
  00000001421AFFE3: mov         rdx,0FFFFFFFFFFFFFFFFh
  00000001421AFFEA: cmovbe      rdx,rax
  00000001421AFFEE: movss       xmm2,dword ptr [__real@3e000000]
  00000001421AFFF6: divss       xmm2,xmm11
  00000001421AFFFB: lea         rax,[142FF80F0h]
  00000001421B0002: mov         qword ptr [rsp+20h],rax
  00000001421B0007: lea         rcx,[rbp+2B8h]
  00000001421B000E: lea         r9,[rbp+3A8h]
  00000001421B0015: call        _ZN5utils13random_splits17h445a73628407a507E
  00000001421B001A: mov         r15,qword ptr [rbp+2C8h]
  00000001421B0021: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B0029: movaps      xmm2,xmm6
  00000001421B002C: movss       dword ptr [rbp+2F0h],xmm11
  00000001421B0035: divss       xmm2,xmm11
  00000001421B003A: movsldup    xmm0,xmm2
  00000001421B003E: mulps       xmm9,xmm0
  00000001421B0042: movshdup    xmm0,xmm9
  00000001421B0047: movaps      xmm3,xmm0
  00000001421B004A: mulss       xmm3,xmm7
  00000001421B004E: movaps      xmm4,xmm9
  00000001421B0052: mulss       xmm4,xmm7
  00000001421B0056: movaps      xmm1,xmm9
  00000001421B005A: subss       xmm1,xmm3
  00000001421B005E: movaps      xmm5,xmm1
  00000001421B0061: mulss       xmm5,xmm1
  00000001421B0065: mulss       xmm10,xmm2
  00000001421B006A: subss       xmm3,xmm10
  00000001421B006F: movaps      xmm14,xmm10
  00000001421B0073: mulss       xmm14,xmm7
  00000001421B0078: subss       xmm14,xmm4
  00000001421B007D: movaps      xmm2,xmm3
  00000001421B0080: mulss       xmm2,xmm3
  00000001421B0084: movaps      xmm4,xmm14
  00000001421B0088: mulss       xmm4,xmm14
  00000001421B008D: addss       xmm4,xmm2
  00000001421B0091: addss       xmm4,xmm5
  00000001421B0095: sqrtss      xmm4,xmm4
  00000001421B0099: movaps      xmm2,xmm6
  00000001421B009C: divss       xmm2,xmm4
  00000001421B00A0: mulss       xmm3,xmm2
  00000001421B00A4: subss       xmm0,xmm3
  00000001421B00A8: xor         eax,eax
  00000001421B00AA: ucomiss     xmm7,xmm0
  00000001421B00AD: mulss       xmm14,xmm2
  00000001421B00B2: setb        cl
  00000001421B00B5: movaps      xmm5,xmm6
  00000001421B00B8: subss       xmm5,xmm0
  00000001421B00BC: xorps       xmm4,xmm4
  00000001421B00BF: sqrtss      xmm4,xmm5
  00000001421B00C3: movss       xmm11,dword ptr [__real@3f000000]
  00000001421B00CC: movaps      xmm3,xmm11
  00000001421B00D0: divss       xmm3,xmm4
  00000001421B00D4: addss       xmm14,xmm9
  00000001421B00D9: addss       xmm0,xmm6
  00000001421B00DD: xorps       xmm6,xmm6
  00000001421B00E0: sqrtss      xmm6,xmm0
  00000001421B00E4: movaps      xmm4,xmm11
  00000001421B00E8: divss       xmm4,xmm6
  00000001421B00EC: jb          00000001421B0116
  00000001421B00EE: mulss       xmm5,xmm3
  00000001421B00F2: movaps      xmm0,xmm14
  00000001421B00F6: mulss       xmm0,xmm3
  00000001421B00FA: movaps      xmm14,xmm5
  00000001421B00FE: mov         r8,qword ptr [rbp+2C0h]
  00000001421B0105: test        r15,r15
  00000001421B0108: mov         qword ptr [rbp+2A0h],r8
  00000001421B010F: jne         00000001421B0136
  00000001421B0111: jmp         00000001421B060B
  00000001421B0116: mulss       xmm14,xmm4
  00000001421B011B: mulss       xmm0,xmm4
  00000001421B011F: mov         r8,qword ptr [rbp+2C0h]
  00000001421B0126: test        r15,r15
  00000001421B0129: mov         qword ptr [rbp+2A0h],r8
  00000001421B0130: je          00000001421B060B
  00000001421B0136: cmp         r15,1
  00000001421B013A: je          00000001421B060B
  00000001421B0140: movss       dword ptr [rbp+240h],xmm15
  00000001421B0149: movss       dword ptr [rbp+380h],xmm13
  00000001421B0152: movaps      xmmword ptr [rbp+390h],xmm12
  00000001421B015A: mov         rdx,qword ptr [rbp+428h]
  00000001421B0161: movss       xmm6,dword ptr [rdx+30h]
  00000001421B0166: movss       xmm5,dword ptr [rdx+2Ch]
  00000001421B016B: mulss       xmm5,xmm6
  00000001421B016F: movss       dword ptr [rbp+250h],xmm5
  00000001421B0177: mulss       xmm1,xmm2
  00000001421B017B: xorps       xmm2,xmm2
  00000001421B017E: subss       xmm2,xmm1
  00000001421B0182: unpcklps    xmm1,xmm10
  00000001421B0186: movaps      xmm5,xmmword ptr [__xmm@00000000000000003f80000000000000]
  00000001421B018D: addps       xmm5,xmm1
  00000001421B0190: shufps      xmm2,xmm5,10h
  00000001421B0194: addps       xmm1,xmmword ptr [__xmm@0000000000000000bf80000000000000]
  00000001421B019B: movsldup    xmm3,xmm3
  00000001421B019F: mulps       xmm3,xmm1
  00000001421B01A2: shufps      xmm2,xmm5,0E2h
  00000001421B01A6: movsldup    xmm1,xmm4
  00000001421B01AA: mulps       xmm1,xmm2
  00000001421B01AD: mov         al,cl
  00000001421B01AF: movd        xmm2,eax
  00000001421B01B3: pshufd      xmm2,xmm2,50h
  00000001421B01B8: pslld       xmm2,1Fh
  00000001421B01BD: psrad       xmm2,1Fh
  00000001421B01C2: andps       xmm1,xmm2
  00000001421B01C5: andnps      xmm2,xmm3
  00000001421B01C8: orps        xmm2,xmm1
  00000001421B01CB: unpcklps    xmm14,xmm0
  00000001421B01CF: movlhps     xmm14,xmm2
  00000001421B01D3: movsd       xmm0,mmword ptr [r8]
  00000001421B01D8: movaps      xmmword ptr [rbp+300h],xmm0
  00000001421B01DF: movaps      xmmword ptr [rbp+100h],xmm14
  00000001421B01E7: lea         rcx,[rbp+100h]
  00000001421B01EE: call        _ZN69_$LT$glam..f32..sse2..quat..Quat$u20$as$u20$utils..halver..Halver$GT$6to_f1617h7d373b4258af3426E
  00000001421B01F3: mov         rsi,rax
  00000001421B01F6: movss       xmm2,dword ptr [__real@3f800000]
  00000001421B01FE: movaps      xmm11,xmm2
  00000001421B0202: subss       xmm11,dword ptr [rbp+250h]
  00000001421B020B: movss       xmm0,dword ptr [__real@bf4ccccd]
  00000001421B0213: addss       xmm0,xmm11
  00000001421B0218: divss       xmm0,dword ptr [__real@3e4ccccc]
  00000001421B0220: xorps       xmm1,xmm1
  00000001421B0223: maxss       xmm1,xmm0
  00000001421B0227: movaps      xmm15,xmm2
  00000001421B022B: minss       xmm15,xmm1
  00000001421B0230: movaps      xmm0,xmm15
  00000001421B0234: addss       xmm0,xmm15
  00000001421B0239: movss       xmm1,dword ptr [__real@40400000]
  00000001421B0241: subss       xmm1,xmm0
  00000001421B0245: mulss       xmm15,xmm15
  00000001421B024A: mulss       xmm15,xmm1
  00000001421B024F: movaps      xmm0,xmm2
  00000001421B0252: subss       xmm0,xmm15
  00000001421B0257: mulss       xmm0,xmm7
  00000001421B025B: mulss       xmm15,dword ptr [__real@3e19999a]
  00000001421B0264: addss       xmm15,xmm0
  00000001421B0269: addss       xmm15,dword ptr [__real@3ecccccd]
  00000001421B0272: movaps      xmm0,xmmword ptr [rbp+300h]
  00000001421B0279: movshdup    xmm13,xmm0
  00000001421B027E: subss       xmm13,xmm0
  00000001421B0283: mulss       xmm13,dword ptr [rbp+2F0h]
  00000001421B028C: movss       xmm0,dword ptr [__real@3fb33333]
  00000001421B0294: mulss       xmm0,xmm13
  00000001421B0299: movss       dword ptr [rbp+280h],xmm15
  00000001421B02A2: movss       dword ptr [rbp+284h],xmm0
  00000001421B02AA: mov         dword ptr [rbp+288h],3E19999Ah
  00000001421B02B4: lea         rcx,[rbp+280h]
  00000001421B02BB: call        _ZN66_$LT$$u5b$f32$u3b$$u20$3$u5d$$u20$as$u20$utils..halver..Halver$GT$6to_f1617ha8b503566bb8cdf3E
  00000001421B02C0: mov         rdi,rax
  00000001421B02C3: movss       xmm12,dword ptr [__real@3f800000]
  00000001421B02CC: movaps      xmm1,xmmword ptr [rbp+300h]
  00000001421B02D3: subss       xmm12,xmm1
  00000001421B02D8: movss       xmm0,dword ptr [__real@3f666666]
  00000001421B02E0: mulss       xmm0,xmm12
  00000001421B02E5: addss       xmm0,xmm1
  00000001421B02E9: call        _ZN45_$LT$f32$u20$as$u20$utils..halver..Halver$GT$6to_f1617h628d79fa31310e7aE
  00000001421B02EE: movss       xmm0,dword ptr [__real@3f800000]
  00000001421B02F6: subss       xmm0,xmm6
  00000001421B02FA: mulss       xmm11,dword ptr [__real@3dcccccd]
  00000001421B0303: movss       xmm1,dword ptr [rbp+250h]
  00000001421B030B: mulss       xmm1,dword ptr [__real@3e19999a]
  00000001421B0313: addss       xmm1,xmm11
  00000001421B0318: mulss       xmm0,dword ptr [__real@3ca3d70a]
  00000001421B0320: mulss       xmm6,dword ptr [__real@3da3d70a]
  00000001421B0328: addss       xmm6,xmm0
  00000001421B032C: addss       xmm6,xmm1
  00000001421B0330: mulss       xmm7,xmm6
  00000001421B0334: movss       xmm0,dword ptr [rbp+380h]
  00000001421B033C: mulss       xmm0,xmm12
  00000001421B0341: movss       xmm1,dword ptr [rbp+240h]
  00000001421B0349: movaps      xmm2,xmmword ptr [rbp+300h]
  00000001421B0350: mulss       xmm1,xmm2
  00000001421B0354: addss       xmm1,xmm0
  00000001421B0358: mulss       xmm13,dword ptr [__real@3f000000]
  00000001421B0361: movaps      xmm0,xmm10
  00000001421B0365: mulss       xmm0,xmm13
  00000001421B036A: addss       xmm0,xmm1
  00000001421B036E: movsldup    xmm1,xmm12
  00000001421B0373: mulps       xmm1,xmmword ptr [rbp+390h]
  00000001421B037A: movsldup    xmm2,xmm2
  00000001421B037E: mulps       xmm2,xmm8
  00000001421B0382: addps       xmm2,xmm1
  00000001421B0385: movsldup    xmm1,xmm13
  00000001421B038A: mulps       xmm1,xmm9
  00000001421B038E: addps       xmm1,xmm2
  00000001421B0391: movaps      xmm13,xmm7
  00000001421B0395: unpcklps    xmm13,xmm6
  00000001421B0399: subps       xmm1,xmm13
  00000001421B039D: subss       xmm0,xmm7
  00000001421B03A1: movlps      qword ptr [rbp+430h],xmm1
  00000001421B03A8: movss       dword ptr [rbp+438h],xmm0
  00000001421B03B0: mov         dword ptr [rbp+43Ch],0
  00000001421B03BA: mov         rcx,7BFF00003C000000h
  00000001421B03C4: mov         qword ptr [rbp+440h],rcx
  00000001421B03CB: mov         dword ptr [rbp+448h],edi
  00000001421B03D1: shr         rdi,20h
  00000001421B03D5: mov         word ptr [rbp+44Ch],di
  00000001421B03DC: mov         word ptr [rbp+44Eh],ax
  00000001421B03E3: mov         qword ptr [rbp+450h],rsi
  00000001421B03EA: mov         eax,dword ptr [rbp+404h]
  00000001421B03F0: mov         dword ptr [rbp+458h],eax
  00000001421B03F6: mov         qword ptr [rbp+45Ch],0
  00000001421B0401: mov         eax,dword ptr [rbp+474h]
  00000001421B0407: mov         dword ptr [rbp+464h],eax
  00000001421B040D: mov         rax,200000000h
  00000001421B0417: mov         qword ptr [rbp+468h],rax
  00000001421B041E: lea         rdx,[rbp+430h]
  00000001421B0425: mov         rcx,qword ptr [rbp+5A0h]
  00000001421B042C: call        00000001421B2CA0
  00000001421B0431: inc         dword ptr [rbp+474h]
  00000001421B0437: cmp         r15,2
  00000001421B043B: je          00000001421B060B
  00000001421B0441: mov         rax,qword ptr [rbp+2A0h]
  00000001421B0448: lea         r13,[rax+r15*4]
  00000001421B044C: add         r15,0FFFFFFFFFFFFFFFEh
  00000001421B0450: lea         rbx,[rax+4]
  00000001421B0454: add         rax,8
  00000001421B0458: mov         esi,1
  00000001421B045D: lea         r12,[rbp+430h]
  00000001421B0464: nop         word ptr cs:[rax+rax]
  00000001421B0470: movsd       xmm11,mmword ptr [rbx]
  00000001421B0475: mov         rbx,rax
  00000001421B0478: movshdup    xmm6,xmm11
  00000001421B047D: subss       xmm6,xmm11
  00000001421B0482: mulss       xmm6,dword ptr [rbp+2F0h]
  00000001421B048A: movaps      xmm12,xmm6
  00000001421B048E: mulss       xmm12,dword ptr [__real@3f000000]
  00000001421B0497: movaps      xmmword ptr [rbp+100h],xmm14
  00000001421B049F: lea         rcx,[rbp+100h]
  00000001421B04A6: call        _ZN69_$LT$glam..f32..sse2..quat..Quat$u20$as$u20$utils..halver..Halver$GT$6to_f1617h7d373b4258af3426E
  00000001421B04AB: mov         rdi,rax
  00000001421B04AE: mulss       xmm6,dword ptr [__real@3fb33333]
  00000001421B04B6: movss       dword ptr [rbp+280h],xmm15
  00000001421B04BF: movss       dword ptr [rbp+284h],xmm6
  00000001421B04C7: mov         dword ptr [rbp+288h],3E19999Ah
  00000001421B04D1: lea         rcx,[rbp+280h]
  00000001421B04D8: call        _ZN66_$LT$$u5b$f32$u3b$$u20$3$u5d$$u20$as$u20$utils..halver..Halver$GT$6to_f1617ha8b503566bb8cdf3E
  00000001421B04DD: mov         r14,rax
  00000001421B04E0: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B04E8: subss       xmm6,xmm11
  00000001421B04ED: movaps      xmm0,xmm6
  00000001421B04F0: mulss       xmm0,dword ptr [__real@3f666666]
  00000001421B04F8: addss       xmm0,xmm11
  00000001421B04FD: call        _ZN45_$LT$f32$u20$as$u20$utils..halver..Halver$GT$6to_f1617h628d79fa31310e7aE
  00000001421B0502: movss       xmm0,dword ptr [rbp+380h]
  00000001421B050A: mulss       xmm0,xmm6
  00000001421B050E: movss       xmm1,dword ptr [rbp+240h]
  00000001421B0516: mulss       xmm1,xmm11
  00000001421B051B: addss       xmm1,xmm0
  00000001421B051F: movaps      xmm0,xmm10
  00000001421B0523: mulss       xmm0,xmm12
  00000001421B0528: addss       xmm0,xmm1
  00000001421B052C: movsldup    xmm1,xmm6
  00000001421B0530: mulps       xmm1,xmmword ptr [rbp+390h]
  00000001421B0537: movsldup    xmm2,xmm11
  00000001421B053C: mulps       xmm2,xmm8
  00000001421B0540: addps       xmm2,xmm1
  00000001421B0543: movsldup    xmm1,xmm12
  00000001421B0548: mulps       xmm1,xmm9
  00000001421B054C: addps       xmm1,xmm2
  00000001421B054F: subps       xmm1,xmm13
  00000001421B0553: subss       xmm0,xmm7
  00000001421B0557: cmp         rsi,r15
  00000001421B055A: sete        cl
  00000001421B055D: or          cl,byte ptr [rbp+487h]
  00000001421B0563: movzx       ecx,cl
  00000001421B0566: inc         ecx
  00000001421B0568: movlps      qword ptr [rbp+430h],xmm1
  00000001421B056F: movss       dword ptr [rbp+438h],xmm0
  00000001421B0577: mov         dword ptr [rbp+43Ch],0
  00000001421B0581: mov         rdx,7BFF00003C000000h
  00000001421B058B: mov         qword ptr [rbp+440h],rdx
  00000001421B0592: mov         dword ptr [rbp+448h],r14d
  00000001421B0599: shr         r14,20h
  00000001421B059D: mov         word ptr [rbp+44Ch],r14w
  00000001421B05A5: mov         word ptr [rbp+44Eh],ax
  00000001421B05AC: mov         qword ptr [rbp+450h],rdi
  00000001421B05B3: mov         eax,dword ptr [rbp+404h]
  00000001421B05B9: mov         dword ptr [rbp+458h],eax
  00000001421B05BF: mov         qword ptr [rbp+45Ch],0
  00000001421B05CA: mov         eax,dword ptr [rbp+474h]
  00000001421B05D0: mov         dword ptr [rbp+464h],eax
  00000001421B05D6: mov         dword ptr [rbp+468h],0
  00000001421B05E0: mov         dword ptr [rbp+46Ch],ecx
  00000001421B05E6: mov         rcx,qword ptr [rbp+5A0h]
  00000001421B05ED: mov         rdx,r12
  00000001421B05F0: call        00000001421B2CA0
  00000001421B05F5: lea         rax,[rbx+4]
  00000001421B05F9: inc         rsi
  00000001421B05FC: inc         dword ptr [rbp+474h]
  00000001421B0602: cmp         rax,r13
  00000001421B0605: jne         00000001421B0470
  00000001421B060B: mov         rdx,qword ptr [rbp+2B8h]
  00000001421B0612: test        rdx,rdx
  00000001421B0615: je          00000001421B062D
  00000001421B0617: shl         rdx,2
  00000001421B061B: mov         r8d,4
  00000001421B0621: mov         rcx,qword ptr [rbp+2A0h]
  00000001421B0628: call        __rust_dealloc
  00000001421B062D: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B0635: mov         r15,qword ptr [rbp+48h]
  00000001421B0639: mov         r11,qword ptr [rbp+50h]
  00000001421B063D: lea         r13,[r15+r11*8]
  00000001421B0641: xor         eax,eax
  00000001421B0643: sub         r11,1
  00000001421B0647: cmovb       r11,rax
  00000001421B064B: mov         rcx,qword ptr [rbp+10h]
  00000001421B064F: mov         r14,qword ptr [rbp+18h]
  00000001421B0653: lea         rdi,[rcx+r14*8]
  00000001421B0657: sub         r14,1
  00000001421B065B: cmovb       r14,rax
  00000001421B065F: mov         rax,qword ptr [rbp+428h]
  00000001421B0666: movzx       eax,byte ptr [rax+54h]
  00000001421B066A: mov         byte ptr [rbp+120h],al
  00000001421B0670: mov         rbx,qword ptr [rbp-10h]
  00000001421B0674: mov         rax,qword ptr [rbp-8]
  00000001421B0678: mov         qword ptr [rbp+0E8h],rax
  00000001421B067F: mov         rax,qword ptr [rbp]
  00000001421B0683: mov         qword ptr [rbp+0F0h],rax
  00000001421B068A: movaps      xmm0,xmmword ptr [rbp+180h]
  00000001421B0691: unpcklps    xmm0,xmmword ptr [rbp+160h]
  00000001421B0698: movaps      xmmword ptr [rbp+180h],xmm0
  00000001421B069F: mov         edx,1
  00000001421B06A4: movaps      xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B06AB: xorps       xmm11,xmm11
  00000001421B06AF: mov         qword ptr [rbp+378h],r15
  00000001421B06B6: mov         qword ptr [rbp+3B8h],rcx
  00000001421B06BD: mov         qword ptr [rbp+420h],rcx
  00000001421B06C4: mov         eax,1
  00000001421B06C9: mov         qword ptr [rbp+410h],rax
  00000001421B06D0: mov         qword ptr [rbp+3E0h],0
  00000001421B06DB: mov         qword ptr [rbp+3B0h],r13
  00000001421B06E2: mov         qword ptr [rbp+370h],rdi
  00000001421B06E9: mov         qword ptr [rbp+228h],rbx
  00000001421B06F0: test        bl,1
  00000001421B06F3: jne         00000001421B09F0
  00000001421B06F9: test        r11,r11
  00000001421B06FC: mov         rsi,qword ptr [rbp+3E0h]
  00000001421B0703: mov         rbx,qword ptr [rbp+3B8h]
  00000001421B070A: mov         rax,qword ptr [rbp+378h]
  00000001421B0711: je          00000001421B07B5
  00000001421B0717: dec         r11
  00000001421B071A: cmp         rax,r13
  00000001421B071D: je          00000001421B07C1
  00000001421B0723: add         rax,8
  00000001421B0727: test        rdx,rdx
  00000001421B072A: jne         00000001421B0882
  00000001421B0730: cmp         r15,r13
  00000001421B0733: je          00000001421B07CD
  00000001421B0739: mov         rdx,r15
  00000001421B073C: lea         r15,[rdx+8]
  00000001421B0740: test        r14,r14
  00000001421B0743: je          00000001421B08D1
  00000001421B0749: dec         r14
  00000001421B074C: cmp         rbx,rdi
  00000001421B074F: je          00000001421B0854
  00000001421B0755: lea         rcx,[rbx+8]
  00000001421B0759: mov         r9,qword ptr [rbp+410h]
  00000001421B0760: test        r9,r9
  00000001421B0763: jne         00000001421B0941
  00000001421B0769: mov         r9,qword ptr [rbp+420h]
  00000001421B0770: cmp         r9,rdi
  00000001421B0773: je          00000001421B0867
  00000001421B0779: lea         r8,[r9+8]
  00000001421B077D: mov         qword ptr [rbp+420h],r8
  00000001421B0784: lea         r8,[rsi+1]
  00000001421B0788: mov         r10,qword ptr [rbp+378h]
  00000001421B078F: mov         qword ptr [rbp+478h],r10
  00000001421B0796: mov         qword ptr [rbp+3F0h],0
  00000001421B07A1: mov         qword ptr [rbp+410h],0
  00000001421B07AC: lea         r10,[rbp+208h]
  00000001421B07B3: jmp         00000001421B0805
  00000001421B07B5: mov         qword ptr [rbp+3F0h],rdx
  00000001421B07BC: xor         r11d,r11d
  00000001421B07BF: jmp         00000001421B07E0
  00000001421B07C1: mov         qword ptr [rbp+3F0h],rdx
  00000001421B07C8: mov         rax,r13
  00000001421B07CB: jmp         00000001421B07E0
  00000001421B07CD: mov         qword ptr [rbp+3F0h],0
  00000001421B07D8: nop         dword ptr [rax+rax]
  00000001421B07E0: mov         rcx,rbx
  00000001421B07E3: mov         r8,rsi
  00000001421B07E6: mov         rsi,qword ptr [rbp+3C0h]
  00000001421B07ED: mov         rdx,qword ptr [rbp+418h]
  00000001421B07F4: mov         rbx,qword ptr [rbp+408h]
  00000001421B07FB: lea         r10,[rbp+478h]
  00000001421B0802: xor         r9d,r9d
  00000001421B0805: mov         qword ptr [rbp+408h],rbx
  00000001421B080C: mov         qword ptr [rbp+3C0h],rsi
  00000001421B0813: mov         qword ptr [r10],r9
  00000001421B0816: mov         rbx,qword ptr [rbp+478h]
  00000001421B081D: mov         qword ptr [rbp+3B8h],rcx
  00000001421B0824: mov         qword ptr [rbp+3E0h],r8
  00000001421B082B: mov         qword ptr [rbp+418h],rdx
  00000001421B0832: test        rbx,rbx
  00000001421B0835: je          00000001421B1FE5
  00000001421B083B: mov         rsi,qword ptr [rbp+18h]
  00000001421B083F: mov         rcx,qword ptr [rbp+3C0h]
  00000001421B0846: cmp         rcx,rsi
  00000001421B0849: jb          00000001421B0C2E
  00000001421B084F: jmp         00000001421B2367
  00000001421B0854: mov         qword ptr [rbp+3F0h],0
  00000001421B085F: mov         rcx,rdi
  00000001421B0862: jmp         00000001421B07E3
  00000001421B0867: mov         qword ptr [rbp+3F0h],0
  00000001421B0872: mov         qword ptr [rbp+410h],0
  00000001421B087D: jmp         00000001421B07E3
  00000001421B0882: mov         r9,rdx
  00000001421B0885: sub         r13,r15
  00000001421B0888: shr         r13,3
  00000001421B088C: cmp         r13,rdx
  00000001421B088F: jbe         00000001421B08E7
  00000001421B0891: lea         rdx,[r15+r9*8]
  00000001421B0895: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B089D: mov         r13,qword ptr [rbp+3B0h]
  00000001421B08A4: mov         rdi,qword ptr [rbp+370h]
  00000001421B08AB: movaps      xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B08B2: xorps       xmm11,xmm11
  00000001421B08B6: mov         rsi,qword ptr [rbp+3E0h]
  00000001421B08BD: mov         rbx,qword ptr [rbp+3B8h]
  00000001421B08C4: lea         r15,[rdx+8]
  00000001421B08C8: test        r14,r14
  00000001421B08CB: jne         00000001421B0749
  00000001421B08D1: mov         qword ptr [rbp+3F0h],0
  00000001421B08DC: mov         rcx,rbx
  00000001421B08DF: xor         r14d,r14d
  00000001421B08E2: jmp         00000001421B07E3
  00000001421B08E7: mov         r13,qword ptr [rbp+3B0h]
  00000001421B08EE: mov         r15,r13
  00000001421B08F1: mov         qword ptr [rbp+3F0h],0
  00000001421B08FC: mov         rcx,qword ptr [rbp+3B8h]
  00000001421B0903: mov         r8,qword ptr [rbp+3E0h]
  00000001421B090A: mov         rsi,qword ptr [rbp+3C0h]
  00000001421B0911: mov         rdx,qword ptr [rbp+418h]
  00000001421B0918: mov         rbx,qword ptr [rbp+408h]
  00000001421B091F: lea         r10,[rbp+478h]
  00000001421B0926: xor         r9d,r9d
  00000001421B0929: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B0931: movaps      xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B0938: xorps       xmm11,xmm11
  00000001421B093C: jmp         00000001421B0805
  00000001421B0941: mov         r8,rdi
  00000001421B0944: sub         r8,qword ptr [rbp+420h]
  00000001421B094B: shr         r8,3
  00000001421B094F: cmp         r8,r9
  00000001421B0952: jbe         00000001421B098F
  00000001421B0954: mov         r10,r9
  00000001421B0957: mov         r9,qword ptr [rbp+420h]
  00000001421B095E: lea         r9,[r9+r10*8]
  00000001421B0962: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B096A: mov         r13,qword ptr [rbp+3B0h]
  00000001421B0971: movaps      xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B0978: xorps       xmm11,xmm11
  00000001421B097C: mov         rsi,qword ptr [rbp+3E0h]
  00000001421B0983: mov         rbx,qword ptr [rbp+3B8h]
  00000001421B098A: jmp         00000001421B0779
  00000001421B098F: mov         qword ptr [rbp+3F0h],0
  00000001421B099A: mov         rdx,qword ptr [rbp+370h]
  00000001421B09A1: mov         qword ptr [rbp+420h],rdx
  00000001421B09A8: mov         qword ptr [rbp+410h],0
  00000001421B09B3: mov         r8,qword ptr [rbp+3E0h]
  00000001421B09BA: mov         rsi,qword ptr [rbp+3C0h]
  00000001421B09C1: mov         rdx,qword ptr [rbp+418h]
  00000001421B09C8: mov         rbx,qword ptr [rbp+408h]
  00000001421B09CF: lea         r10,[rbp+478h]
  00000001421B09D6: xor         r9d,r9d
  00000001421B09D9: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B09E1: mov         r13,qword ptr [rbp+3B0h]
  00000001421B09E8: jmp         00000001421B0931
  00000001421B09ED: nop         dword ptr [rax]
  00000001421B09F0: mov         r10,qword ptr [rbp+3E0h]
  00000001421B09F7: mov         rsi,qword ptr [rbp+3B8h]
  00000001421B09FE: mov         rbx,qword ptr [rbp+418h]
  00000001421B0A05: mov         rax,qword ptr [rbp+378h]
  00000001421B0A0C: jmp         00000001421B0A20
  00000001421B0A0E: nop
  00000001421B0A10: mov         rsi,rcx
  00000001421B0A13: mov         r10,r8
  00000001421B0A16: mov         rbx,rdx
  00000001421B0A19: mov         rdx,qword ptr [rbp+3F0h]
  00000001421B0A20: test        r11,r11
  00000001421B0A23: je          00000001421B0AE0
  00000001421B0A29: dec         r11
  00000001421B0A2C: cmp         rax,r13
  00000001421B0A2F: je          00000001421B0AF0
  00000001421B0A35: mov         r12,rax
  00000001421B0A38: add         rax,8
  00000001421B0A3C: test        rdx,rdx
  00000001421B0A3F: jne         00000001421B0B24
  00000001421B0A45: cmp         r15,r13
  00000001421B0A48: je          00000001421B0B5A
  00000001421B0A4E: mov         rdx,r15
  00000001421B0A51: lea         r15,[rdx+8]
  00000001421B0A55: test        r14,r14
  00000001421B0A58: je          00000001421B0B44
  00000001421B0A5E: dec         r14
  00000001421B0A61: cmp         rsi,rdi
  00000001421B0A64: je          00000001421B0AFC
  00000001421B0A6A: lea         rcx,[rsi+8]
  00000001421B0A6E: mov         r9,qword ptr [rbp+410h]
  00000001421B0A75: test        r9,r9
  00000001421B0A78: jne         00000001421B0BB9
  00000001421B0A7E: cmp         qword ptr [rbp+420h],rdi
  00000001421B0A85: je          00000001421B0B0C
  00000001421B0A8B: mov         r9,qword ptr [rbp+420h]
  00000001421B0A92: lea         r8,[r9+8]
  00000001421B0A96: mov         qword ptr [rbp+420h],r8
  00000001421B0A9D: lea         r8,[r10+1]
  00000001421B0AA1: mov         qword ptr [rbp+478h],r12
  00000001421B0AA8: mov         qword ptr [rbp+3F0h],0
  00000001421B0AB3: mov         qword ptr [rbp+410h],0
  00000001421B0ABE: mov         qword ptr [rbp+3C0h],r10
  00000001421B0AC5: mov         qword ptr [rbp+408h],rsi
  00000001421B0ACC: lea         r10,[rbp+208h]
  00000001421B0AD3: jmp         00000001421B0B83
  00000001421B0AD8: nop         dword ptr [rax+rax]
  00000001421B0AE0: mov         qword ptr [rbp+3F0h],rdx
  00000001421B0AE7: xor         r11d,r11d
  00000001421B0AEA: jmp         00000001421B0B70
  00000001421B0AEF: nop
  00000001421B0AF0: mov         qword ptr [rbp+3F0h],rdx
  00000001421B0AF7: mov         rax,r13
  00000001421B0AFA: jmp         00000001421B0B70
  00000001421B0AFC: mov         qword ptr [rbp+3F0h],0
  00000001421B0B07: mov         rcx,rdi
  00000001421B0B0A: jmp         00000001421B0B73
  00000001421B0B0C: mov         qword ptr [rbp+3F0h],0
  00000001421B0B17: mov         qword ptr [rbp+410h],0
  00000001421B0B22: jmp         00000001421B0B73
  00000001421B0B24: mov         rcx,r13
  00000001421B0B27: sub         rcx,r15
  00000001421B0B2A: shr         rcx,3
  00000001421B0B2E: cmp         rcx,rdx
  00000001421B0B31: jbe         00000001421B0B57
  00000001421B0B33: lea         rdx,[r15+rdx*8]
  00000001421B0B37: lea         r15,[rdx+8]
  00000001421B0B3B: test        r14,r14
  00000001421B0B3E: jne         00000001421B0A5E
  00000001421B0B44: mov         qword ptr [rbp+3F0h],0
  00000001421B0B4F: mov         rcx,rsi
  00000001421B0B52: xor         r14d,r14d
  00000001421B0B55: jmp         00000001421B0B73
  00000001421B0B57: mov         r15,r13
  00000001421B0B5A: mov         qword ptr [rbp+3F0h],0
  00000001421B0B65: nop         word ptr cs:[rax+rax]
  00000001421B0B70: mov         rcx,rsi
  00000001421B0B73: mov         r8,r10
  00000001421B0B76: mov         rdx,rbx
  00000001421B0B79: lea         r10,[rbp+478h]
  00000001421B0B80: xor         r9d,r9d
  00000001421B0B83: mov         qword ptr [r10],r9
  00000001421B0B86: mov         rbx,qword ptr [rbp+478h]
  00000001421B0B8D: test        rbx,rbx
  00000001421B0B90: je          00000001421B1FE5
  00000001421B0B96: mov         r9,qword ptr [rbp+3C0h]
  00000001421B0B9D: cmp         qword ptr [rbp+0E8h],r9
  00000001421B0BA4: je          00000001421B0A10
  00000001421B0BAA: cmp         qword ptr [rbp+0F0h],r9
  00000001421B0BB1: je          00000001421B0A10
  00000001421B0BB7: jmp         00000001421B0C05
  00000001421B0BB9: mov         r8,rdi
  00000001421B0BBC: sub         r8,qword ptr [rbp+420h]
  00000001421B0BC3: shr         r8,3
  00000001421B0BC7: cmp         r8,r9
  00000001421B0BCA: jbe         00000001421B0BE3
  00000001421B0BCC: mov         r8,qword ptr [rbp+420h]
  00000001421B0BD3: lea         r8,[r8+r9*8]
  00000001421B0BD7: mov         qword ptr [rbp+420h],r8
  00000001421B0BDE: jmp         00000001421B0A8B
  00000001421B0BE3: mov         qword ptr [rbp+3F0h],0
  00000001421B0BEE: mov         qword ptr [rbp+420h],rdi
  00000001421B0BF5: mov         qword ptr [rbp+410h],0
  00000001421B0C00: jmp         00000001421B0B73
  00000001421B0C05: mov         qword ptr [rbp+3B8h],rcx
  00000001421B0C0C: mov         qword ptr [rbp+3E0h],r8
  00000001421B0C13: mov         qword ptr [rbp+418h],rdx
  00000001421B0C1A: mov         rsi,qword ptr [rbp+18h]
  00000001421B0C1E: mov         rcx,qword ptr [rbp+3C0h]
  00000001421B0C25: cmp         rcx,rsi
  00000001421B0C28: jae         00000001421B2367
  00000001421B0C2E: mov         qword ptr [rbp+378h],rax
  00000001421B0C35: mov         qword ptr [rbp+220h],r15
  00000001421B0C3C: mov         qword ptr [rbp+218h],r14
  00000001421B0C43: mov         qword ptr [rbp+0F8h],r11
  00000001421B0C4A: mov         rdi,qword ptr [rbp+50h]
  00000001421B0C4E: cmp         rcx,rdi
  00000001421B0C51: jae         00000001421B2378
  00000001421B0C57: mov         r14,qword ptr [rbp+208h]
  00000001421B0C5E: mov         r15,qword ptr [rbp+48h]
  00000001421B0C62: mov         r12,qword ptr [rbp+10h]
  00000001421B0C66: movsd       xmm0,mmword ptr [r12+rcx*8]
  00000001421B0C6C: movsd       xmm1,mmword ptr [r15+rcx*8]
  00000001421B0C72: subps       xmm0,xmm1
  00000001421B0C75: unpcklps    xmm0,xmm0
  00000001421B0C78: movaps      xmm1,xmmword ptr [rbp+110h]
  00000001421B0C7F: movaps      xmmword ptr [rbp+260h],xmm1
  00000001421B0C86: mulps       xmm0,xmm1
  00000001421B0C89: movaps      xmm1,xmm0
  00000001421B0C8C: unpckhpd    xmm1,xmm0
  00000001421B0C90: addps       xmm1,xmm0
  00000001421B0C93: movaps      xmm0,xmm1
  00000001421B0C96: mulps       xmm0,xmm1
  00000001421B0C99: movshdup    xmm2,xmm0
  00000001421B0C9D: addss       xmm2,xmm0
  00000001421B0CA1: xorps       xmm0,xmm0
  00000001421B0CA4: sqrtss      xmm0,xmm2
  00000001421B0CA8: movaps      xmm2,xmm6
  00000001421B0CAB: divss       xmm2,xmm0
  00000001421B0CAF: movaps      xmm0,xmm2
  00000001421B0CB2: xorps       xmm0,xmm3
  00000001421B0CB5: unpcklps    xmm0,xmm2
  00000001421B0CB8: shufps      xmm1,xmm1,0E1h
  00000001421B0CBC: mulps       xmm1,xmm0
  00000001421B0CBF: movlps      qword ptr [rbp+430h],xmm1
  00000001421B0CC6: lea         rcx,[rbp+9Ch]
  00000001421B0CCD: lea         rdx,[rbp+430h]
  00000001421B0CD4: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421B0CD9: mov         rax,qword ptr [rbp+3C0h]
  00000001421B0CE0: movsd       xmm0,mmword ptr [r15+rax*8]
  00000001421B0CE6: unpcklps    xmm0,xmm0
  00000001421B0CE9: mulps       xmm0,xmmword ptr [rbp+260h]
  00000001421B0CF0: movaps      xmm1,xmm0
  00000001421B0CF3: unpckhpd    xmm1,xmm0
  00000001421B0CF7: addps       xmm1,xmm0
  00000001421B0CFA: mov         rax,qword ptr [rbp+3E8h]
  00000001421B0D01: movsd       xmm0,mmword ptr [rax+8]
  00000001421B0D06: addps       xmm0,xmm1
  00000001421B0D09: movlps      qword ptr [rbp+430h],xmm0
  00000001421B0D10: lea         rcx,[rbp+90h]
  00000001421B0D17: lea         rdx,[rbp+430h]
  00000001421B0D1E: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421B0D23: mov         rax,qword ptr [rbp+3C0h]
  00000001421B0D2A: lea         rcx,[rax+1]
  00000001421B0D2E: cmp         rcx,rdi
  00000001421B0D31: jae         00000001421B2389
  00000001421B0D37: cmp         rcx,rsi
  00000001421B0D3A: movaps      xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B0D41: jae         00000001421B239A
  00000001421B0D47: movss       xmm6,dword ptr [rbp+9Ch]
  00000001421B0D4F: movsd       xmm7,mmword ptr [rbp+0A0h]
  00000001421B0D57: movss       xmm8,dword ptr [rbp+90h]
  00000001421B0D60: mulss       xmm8,xmm6
  00000001421B0D65: movsd       xmm9,mmword ptr [rbp+94h]
  00000001421B0D6E: movsd       xmm0,mmword ptr [r15+rax*8+8]
  00000001421B0D75: movsd       xmm1,mmword ptr [r12+rax*8+8]
  00000001421B0D7C: subps       xmm0,xmm1
  00000001421B0D7F: unpcklps    xmm0,xmm0
  00000001421B0D82: mulps       xmm0,xmmword ptr [rbp+260h]
  00000001421B0D89: movaps      xmm1,xmm0
  00000001421B0D8C: unpckhpd    xmm1,xmm0
  00000001421B0D90: addps       xmm1,xmm0
  00000001421B0D93: movaps      xmm0,xmm1
  00000001421B0D96: mulps       xmm0,xmm1
  00000001421B0D99: movshdup    xmm2,xmm0
  00000001421B0D9D: addss       xmm2,xmm0
  00000001421B0DA1: xorps       xmm0,xmm0
  00000001421B0DA4: sqrtss      xmm0,xmm2
  00000001421B0DA8: movss       xmm2,dword ptr [__real@3f800000]
  00000001421B0DB0: divss       xmm2,xmm0
  00000001421B0DB4: movaps      xmm0,xmm2
  00000001421B0DB7: xorps       xmm0,xmm3
  00000001421B0DBA: unpcklps    xmm0,xmm2
  00000001421B0DBD: shufps      xmm1,xmm1,0E1h
  00000001421B0DC1: mulps       xmm1,xmm0
  00000001421B0DC4: movlps      qword ptr [rbp+430h],xmm1
  00000001421B0DCB: lea         rcx,[rbp+84h]
  00000001421B0DD2: lea         rdx,[rbp+430h]
  00000001421B0DD9: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421B0DDE: mov         rax,qword ptr [rbp+3C0h]
  00000001421B0DE5: movsd       xmm0,mmword ptr [r15+rax*8+8]
  00000001421B0DEC: unpcklps    xmm0,xmm0
  00000001421B0DEF: mulps       xmm0,xmmword ptr [rbp+260h]
  00000001421B0DF6: movaps      xmm1,xmm0
  00000001421B0DF9: unpckhpd    xmm1,xmm0
  00000001421B0DFD: addps       xmm1,xmm0
  00000001421B0E00: mov         rax,qword ptr [rbp+3E8h]
  00000001421B0E07: movsd       xmm0,mmword ptr [rax+8]
  00000001421B0E0C: addps       xmm0,xmm1
  00000001421B0E0F: movlps      qword ptr [rbp+430h],xmm0
  00000001421B0E16: lea         rcx,[rbp+78h]
  00000001421B0E1A: lea         rdx,[rbp+430h]
  00000001421B0E21: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421B0E26: mulps       xmm9,xmm7
  00000001421B0E2A: addss       xmm8,xmm9
  00000001421B0E2F: movshdup    xmm0,xmm9
  00000001421B0E34: addss       xmm0,xmm8
  00000001421B0E39: movss       xmm4,dword ptr [__real@bd99999a]
  00000001421B0E41: movaps      xmm1,xmm4
  00000001421B0E44: subss       xmm1,xmm0
  00000001421B0E48: movlhps     xmm6,xmm7
  00000001421B0E4B: shufps      xmm1,xmm7,0D4h
  00000001421B0E4F: shufps      xmm6,xmm1,28h
  00000001421B0E53: movss       xmm0,dword ptr [rbp+84h]
  00000001421B0E5B: movsd       xmm1,mmword ptr [rbp+88h]
  00000001421B0E63: movss       xmm2,dword ptr [rbp+78h]
  00000001421B0E68: mulss       xmm2,xmm0
  00000001421B0E6C: movsd       xmm3,mmword ptr [rbp+7Ch]
  00000001421B0E71: mulps       xmm3,xmm1
  00000001421B0E74: addss       xmm2,xmm3
  00000001421B0E78: movshdup    xmm3,xmm3
  00000001421B0E7C: addss       xmm3,xmm2
  00000001421B0E80: movaps      xmm2,xmm4
  00000001421B0E83: subss       xmm2,xmm3
  00000001421B0E87: movlhps     xmm0,xmm1
  00000001421B0E8A: shufps      xmm2,xmm1,0D4h
  00000001421B0E8E: shufps      xmm0,xmm2,28h
  00000001421B0E92: movaps      xmmword ptr [rbp+280h],xmm6
  00000001421B0E99: movaps      xmmword ptr [rbp+290h],xmm0
  00000001421B0EA0: movsd       xmm6,mmword ptr [rbx]
  00000001421B0EA4: mov         rax,qword ptr [rbp+418h]
  00000001421B0EAB: movsd       xmm7,mmword ptr [rax]
  00000001421B0EAF: mov         rax,qword ptr [rbp+408h]
  00000001421B0EB6: movsd       xmm8,mmword ptr [rax]
  00000001421B0EBB: movsd       xmm9,mmword ptr [r14]
  00000001421B0EC0: mov         rcx,qword ptr [rbp+428h]
  00000001421B0EC7: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  00000001421B0ECC: addps       xmm8,xmm9
  00000001421B0ED0: movaps      xmm5,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B0ED7: mulps       xmm8,xmm5
  00000001421B0EDB: movaps      xmm4,xmmword ptr [rbp+180h]
  00000001421B0EE2: movaps      xmm1,xmm4
  00000001421B0EE5: subps       xmm1,xmm8
  00000001421B0EE9: mulps       xmm1,xmm1
  00000001421B0EEC: movshdup    xmm2,xmm1
  00000001421B0EF0: addss       xmm2,xmm1
  00000001421B0EF4: xorps       xmm3,xmm3
  00000001421B0EF7: sqrtss      xmm3,xmm2
  00000001421B0EFB: addps       xmm6,xmm7
  00000001421B0EFE: mulps       xmm6,xmm5
  00000001421B0F01: movaps      xmm1,xmm4
  00000001421B0F04: subps       xmm1,xmm6
  00000001421B0F07: mulps       xmm1,xmm1
  00000001421B0F0A: movshdup    xmm2,xmm1
  00000001421B0F0E: addss       xmm2,xmm1
  00000001421B0F12: sqrtss      xmm2,xmm2
  00000001421B0F16: movss       dword ptr [rsp+20h],xmm0
  00000001421B0F1C: lea         rcx,[rbp+2B8h]
  00000001421B0F23: lea         rdx,[rbp+190h]
  00000001421B0F2A: call        _ZN12country_core9resources5roofs10roof_shape16profile_curve_ws17h83456c52e1995788E
  00000001421B0F2F: movss       xmm0,dword ptr [rbp+2E8h]
  00000001421B0F37: divss       xmm0,dword ptr [__real@3ee00000]
  00000001421B0F3F: call        ceilf
  00000001421B0F44: maxss       xmm0,dword ptr [__real@40000000]
  00000001421B0F4C: cvttss2si   eax,xmm0
  00000001421B0F50: ucomiss     xmm0,dword ptr [__real@4effffff]
  00000001421B0F57: mov         ecx,7FFFFFFFh
  00000001421B0F5C: cmova       eax,ecx
  00000001421B0F5F: movss       dword ptr [rbp+3A4h],xmm0
  00000001421B0F67: ucomiss     xmm0,xmm0
  00000001421B0F6A: mov         ecx,0
  00000001421B0F6F: cmovp       eax,ecx
  00000001421B0F72: dec         eax
  00000001421B0F74: lea         rcx,[rbp+3A4h]
  00000001421B0F7B: mov         qword ptr [rbp+430h],rcx
  00000001421B0F82: lea         rcx,[rbp+190h]
  00000001421B0F89: mov         qword ptr [rbp+438h],rcx
  00000001421B0F90: mov         qword ptr [rbp+440h],rbx
  00000001421B0F97: mov         rcx,qword ptr [rbp+408h]
  00000001421B0F9E: mov         qword ptr [rbp+448h],rcx
  00000001421B0FA5: mov         rcx,qword ptr [rbp+418h]
  00000001421B0FAC: mov         qword ptr [rbp+450h],rcx
  00000001421B0FB3: mov         qword ptr [rbp+458h],r14
  00000001421B0FBA: mov         rcx,qword ptr [rbp+428h]
  00000001421B0FC1: mov         qword ptr [rbp+460h],rcx
  00000001421B0FC8: mov         dword ptr [rbp+468h],0
  00000001421B0FD2: mov         dword ptr [rbp+46Ch],eax
  00000001421B0FD8: lea         rcx,[rbp+310h]
  00000001421B0FDF: lea         rdx,[rbp+430h]
  00000001421B0FE6: lea         r8,[142FF7FE0h]
  00000001421B0FED: call        _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17h9f8e7fe9a1e4a634E
  00000001421B0FF2: mov         rsi,qword ptr [rbp+320h]
  00000001421B0FF9: test        rsi,rsi
  00000001421B0FFC: mov         rdi,qword ptr [rbp+370h]
  00000001421B1003: mov         rbx,qword ptr [rbp+228h]
  00000001421B100A: mov         r14,qword ptr [rbp+218h]
  00000001421B1011: mov         r15,qword ptr [rbp+220h]
  00000001421B1018: je          00000001421B2359
  00000001421B101E: mov         rax,qword ptr [rbp+318h]
  00000001421B1025: lea         rcx,[rsi+rsi*2]
  00000001421B1029: lea         rcx,[rax+rcx*8]
  00000001421B102D: add         rcx,0FFFFFFFFFFFFFFE8h
  00000001421B1031: test        rcx,rcx
  00000001421B1034: je          00000001421B2359
  00000001421B103A: mov         rdx,qword ptr [rcx+10h]
  00000001421B103E: mov         qword ptr [rbp+440h],rdx
  00000001421B1045: movups      xmm0,xmmword ptr [rcx]
  00000001421B1048: movaps      xmmword ptr [rbp+430h],xmm0
  00000001421B104F: cmp         rsi,qword ptr [rbp+310h]
  00000001421B1056: jne         00000001421B1072
  00000001421B1058: lea         rcx,[rbp+310h]
  00000001421B105F: lea         rdx,[142FF8180h]
  00000001421B1066: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h190ed917e37517a0E
  00000001421B106B: mov         rax,qword ptr [rbp+318h]
  00000001421B1072: mov         r12,r13
  00000001421B1075: lea         rcx,[rsi*8]
  00000001421B107D: lea         rcx,[rcx+rcx*2]
  00000001421B1081: mov         rdx,qword ptr [rbp+440h]
  00000001421B1088: mov         qword ptr [rax+rcx+10h],rdx
  00000001421B108D: movaps      xmm0,xmmword ptr [rbp+430h]
  00000001421B1094: movups      xmmword ptr [rax+rcx],xmm0
  00000001421B1098: inc         rsi
  00000001421B109B: mov         qword ptr [rbp+320h],rsi
  00000001421B10A2: mov         rax,qword ptr [rbp+318h]
  00000001421B10A9: add         rcx,rax
  00000001421B10AC: add         rcx,18h
  00000001421B10B0: mov         qword ptr [rbp+278h],rcx
  00000001421B10B7: mov         cl,1
  00000001421B10B9: xor         r13d,r13d
  00000001421B10BC: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B10C4: jmp         00000001421B10DC
  00000001421B10C6: nop         word ptr cs:[rax+rax]
  00000001421B10D0: xor         ecx,ecx
  00000001421B10D2: mov         rax,rsi
  00000001421B10D5: mov         r13,qword ptr [rbp+170h]
  00000001421B10DC: cmp         rax,qword ptr [rbp+278h]
  00000001421B10E3: je          00000001421B1F55
  00000001421B10E9: lea         rdx,[rax+18h]
  00000001421B10ED: test        cl,1
  00000001421B10F0: je          00000001421B1110
  00000001421B10F2: cmp         rdx,qword ptr [rbp+278h]
  00000001421B10F9: je          00000001421B1F47
  00000001421B10FF: lea         rsi,[rax+30h]
  00000001421B1103: mov         rcx,rax
  00000001421B1106: mov         rax,rdx
  00000001421B1109: jmp         00000001421B111A
  00000001421B110B: nop         dword ptr [rax+rax]
  00000001421B1110: mov         rsi,rdx
  00000001421B1113: mov         rcx,qword ptr [rbp+210h]
  00000001421B111A: movss       xmm0,dword ptr [rcx+10h]
  00000001421B111F: movss       dword ptr [rbp+2A0h],xmm0
  00000001421B1127: movups      xmm6,xmmword ptr [rcx]
  00000001421B112A: movss       xmm12,dword ptr [rcx+14h]
  00000001421B1130: movsd       xmm7,mmword ptr [rax]
  00000001421B1134: movsd       xmm8,mmword ptr [rax+8]
  00000001421B113A: mov         qword ptr [rbp+210h],rax
  00000001421B1141: movss       xmm9,dword ptr [rax+14h]
  00000001421B1147: movddup     xmm10,mmword ptr [rcx+8]
  00000001421B114D: movaps      xmm0,xmm6
  00000001421B1150: subps       xmm0,xmm10
  00000001421B1154: mulps       xmm0,xmm0
  00000001421B1157: movshdup    xmm1,xmm0
  00000001421B115B: addss       xmm1,xmm0
  00000001421B115F: sqrtss      xmm13,xmm1
  00000001421B1164: movaps      xmm0,xmm13
  00000001421B1168: addss       xmm0,xmm13
  00000001421B116D: call        ceilf
  00000001421B1172: maxss       xmm0,dword ptr [__real@40400000]
  00000001421B117A: cvttss2si   eax,xmm0
  00000001421B117E: ucomiss     xmm0,dword ptr [__real@4effffff]
  00000001421B1185: mov         ecx,7FFFFFFFh
  00000001421B118A: cmova       eax,ecx
  00000001421B118D: ucomiss     xmm0,xmm0
  00000001421B1190: mov         ecx,0
  00000001421B1195: cmovp       eax,ecx
  00000001421B1198: movsxd      rdx,eax
  00000001421B119B: inc         rdx
  00000001421B119E: xorps       xmm2,xmm2
  00000001421B11A1: movss       dword ptr [rbp+130h],xmm13
  00000001421B11AA: divss       xmm2,xmm13
  00000001421B11AF: lea         rax,[142FF8198h]
  00000001421B11B6: mov         qword ptr [rsp+20h],rax
  00000001421B11BB: lea         rcx,[rbp+1F0h]
  00000001421B11C2: lea         r9,[rbp+3A8h]
  00000001421B11C9: call        _ZN5utils13random_splits17h445a73628407a507E
  00000001421B11CE: addps       xmm10,xmm6
  00000001421B11D2: mulps       xmm10,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B11DA: movlps      qword ptr [rbp+3D8h],xmm10
  00000001421B11E2: lea         rcx,[rbp+338h]
  00000001421B11E9: lea         rdx,[rbp+3D8h]
  00000001421B11F0: movaps      xmm2,xmm12
  00000001421B11F4: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421B11F9: addps       xmm7,xmm8
  00000001421B11FD: mulps       xmm7,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B1204: movlps      qword ptr [rbp+328h],xmm7
  00000001421B120B: lea         rcx,[rbp+430h]
  00000001421B1212: lea         rdx,[rbp+328h]
  00000001421B1219: movaps      xmm2,xmm9
  00000001421B121D: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421B1222: mov         rdx,qword ptr [rbp+320h]
  00000001421B1229: lea         rax,[rdx-2]
  00000001421B122D: cmp         r13,rax
  00000001421B1230: jne         00000001421B1310
  00000001421B1236: cmp         rdx,1
  00000001421B123A: jbe         00000001421B2348
  00000001421B1240: lea         rcx,[rdx-3]
  00000001421B1244: cmp         rdx,2
  00000001421B1248: je          00000001421B2335
  00000001421B124E: mov         rax,qword ptr [rbp+318h]
  00000001421B1255: lea         rdx,[r13*2]
  00000001421B125D: add         rdx,r13
  00000001421B1260: movss       xmm2,dword ptr [rax+rdx*8+14h]
  00000001421B1266: lea         rcx,[rcx+rcx*2]
  00000001421B126A: movsd       xmm7,mmword ptr [rax+rcx*8]
  00000001421B126F: movsd       xmm8,mmword ptr [rax+rcx*8+8]
  00000001421B1276: movss       xmm9,dword ptr [rax+rcx*8+14h]
  00000001421B127D: movsd       xmm0,mmword ptr [rax+rdx*8]
  00000001421B1282: movsd       xmm1,mmword ptr [rax+rdx*8+8]
  00000001421B1288: addps       xmm1,xmm0
  00000001421B128B: mulps       xmm1,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B1292: movlps      qword ptr [rbp+430h],xmm1
  00000001421B1299: lea         rcx,[rbp+1E4h]
  00000001421B12A0: lea         rdx,[rbp+430h]
  00000001421B12A7: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421B12AC: addps       xmm7,xmm8
  00000001421B12B0: mulps       xmm7,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B12B7: movlps      qword ptr [rbp+430h],xmm7
  00000001421B12BE: lea         rcx,[rbp+1D8h]
  00000001421B12C5: lea         rdx,[rbp+430h]
  00000001421B12CC: movaps      xmm2,xmm9
  00000001421B12D0: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421B12D5: movss       xmm0,dword ptr [rbp+1E8h]
  00000001421B12DD: subss       xmm0,dword ptr [rbp+1DCh]
  00000001421B12E5: mulss       xmm0,xmm0
  00000001421B12E9: movss       xmm1,dword ptr [rbp+1ECh]
  00000001421B12F1: movss       xmm2,dword ptr [rbp+1E4h]
  00000001421B12F9: unpcklps    xmm2,xmm1
  00000001421B12FC: movss       xmm1,dword ptr [rbp+1D8h]
  00000001421B1304: movss       xmm3,dword ptr [rbp+1E0h]
  00000001421B130C: jmp         00000001421B1347
  00000001421B130E: nop
  00000001421B1310: movss       xmm0,dword ptr [rbp+33Ch]
  00000001421B1318: subss       xmm0,dword ptr [rbp+434h]
  00000001421B1320: mulss       xmm0,xmm0
  00000001421B1324: movss       xmm1,dword ptr [rbp+340h]
  00000001421B132C: movss       xmm2,dword ptr [rbp+338h]
  00000001421B1334: unpcklps    xmm2,xmm1
  00000001421B1337: movss       xmm1,dword ptr [rbp+430h]
  00000001421B133F: movss       xmm3,dword ptr [rbp+438h]
  00000001421B1347: unpcklps    xmm1,xmm3
  00000001421B134A: subps       xmm2,xmm1
  00000001421B134D: mulps       xmm2,xmm2
  00000001421B1350: addss       xmm0,xmm2
  00000001421B1354: movshdup    xmm1,xmm2
  00000001421B1358: addss       xmm1,xmm0
  00000001421B135C: xorps       xmm0,xmm0
  00000001421B135F: sqrtss      xmm0,xmm1
  00000001421B1363: movss       dword ptr [rbp+380h],xmm0
  00000001421B136B: mov         rbx,qword ptr [rbp+200h]
  00000001421B1372: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001421B1379: mov         ecx,10h
  00000001421B137E: mov         edx,4
  00000001421B1383: call        __rust_alloc
  00000001421B1388: test        rax,rax
  00000001421B138B: je          00000001421B2324
  00000001421B1391: movups      xmmword ptr [rax],xmm6
  00000001421B1394: mov         qword ptr [rbp+430h],2
  00000001421B139F: mov         qword ptr [rbp+438h],rax
  00000001421B13A6: mov         qword ptr [rbp+440h],2
  00000001421B13B1: lea         rcx,[rbp+338h]
  00000001421B13B8: lea         rdx,[rbp+430h]
  00000001421B13BF: lea         r8,[142FF81E0h]
  00000001421B13C6: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$22crashy_new_from_points17ha4e3814ae7345e81E
  00000001421B13CB: lea         r8,[r13+1]
  00000001421B13CF: mov         rdx,r13
  00000001421B13D2: lea         r13,[rbx-1]
  00000001421B13D6: movss       xmm0,dword ptr [rbp+380h]
  00000001421B13DE: mulss       xmm0,dword ptr [__real@3fb6db6e]
  00000001421B13E6: movss       dword ptr [rbp+380h],xmm0
  00000001421B13EE: mov         rcx,rdx
  00000001421B13F1: shr         rcx,1
  00000001421B13F4: mov         eax,edx
  00000001421B13F6: and         eax,1
  00000001421B13F9: or          rax,rcx
  00000001421B13FC: test        rdx,rdx
  00000001421B13FF: mov         qword ptr [rbp+160h],rsi
  00000001421B1406: mov         qword ptr [rbp+250h],rdx
  00000001421B140D: mov         qword ptr [rbp+170h],r8
  00000001421B1414: js          00000001421B1420
  00000001421B1416: xorps       xmm0,xmm0
  00000001421B1419: cvtsi2ss    xmm0,rdx
  00000001421B141E: jmp         00000001421B142C
  00000001421B1420: xorps       xmm0,xmm0
  00000001421B1423: cvtsi2ss    xmm0,rax
  00000001421B1428: addss       xmm0,xmm0
  00000001421B142C: movss       dword ptr [rbp+300h],xmm0
  00000001421B1434: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B143C: sete        sil
  00000001421B1440: or          sil,byte ptr [rbp+487h]
  00000001421B1447: xor         r12d,r12d
  00000001421B144A: movss       dword ptr [rbp+240h],xmm12
  00000001421B1453: jmp         00000001421B1472
  00000001421B1455: nop         word ptr cs:[rax+rax]
  00000001421B1460: inc         dword ptr [rbp+474h]
  00000001421B1466: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B146E: xorps       xmm11,xmm11
  00000001421B1472: cmp         r12,r13
  00000001421B1475: jae         00000001421B1EA0
  00000001421B147B: cmp         rbx,r12
  00000001421B147E: jbe         00000001421B228F
  00000001421B1484: mov         rax,qword ptr [rbp+1F8h]
  00000001421B148B: movss       xmm9,dword ptr [rax+r12*4]
  00000001421B1491: movss       dword ptr [rbp+3D8h],xmm9
  00000001421B149A: ucomiss     xmm9,xmm11
  00000001421B149E: jb          00000001421B20FB
  00000001421B14A4: movss       xmm0,dword ptr [__real@3f800000]
  00000001421B14AC: ucomiss     xmm0,xmm9
  00000001421B14B0: jb          00000001421B20FB
  00000001421B14B6: ucomiss     xmm9,xmm9
  00000001421B14BA: jp          00000001421B216C
  00000001421B14C0: inc         r12
  00000001421B14C3: cmp         r12,rbx
  00000001421B14C6: jae         00000001421B22A6
  00000001421B14CC: movss       xmm10,dword ptr [rax+r12*4]
  00000001421B14D2: movss       dword ptr [rbp+3D8h],xmm10
  00000001421B14DB: ucomiss     xmm10,xmm11
  00000001421B14DF: jb          00000001421B2189
  00000001421B14E5: movss       xmm0,dword ptr [__real@3f800000]
  00000001421B14ED: ucomiss     xmm0,xmm10
  00000001421B14F1: jb          00000001421B2189
  00000001421B14F7: ucomiss     xmm10,xmm10
  00000001421B14FB: jp          00000001421B21FA
  00000001421B1501: lea         rcx,[rbp+338h]
  00000001421B1508: movaps      xmm1,xmm9
  00000001421B150C: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001421B1511: mov         rdi,qword ptr [rbp+348h]
  00000001421B1518: mov         ecx,eax
  00000001421B151A: cmp         rdi,rcx
  00000001421B151D: jbe         00000001421B22BD
  00000001421B1523: lea         rax,[rcx+1]
  00000001421B1527: cmp         rax,rdi
  00000001421B152A: jae         00000001421B22D1
  00000001421B1530: movaps      xmm11,xmm0
  00000001421B1534: mov         r14,qword ptr [rbp+340h]
  00000001421B153B: movsd       xmm8,mmword ptr [r14+rcx*8]
  00000001421B1541: movsd       xmm7,mmword ptr [r14+rcx*8+8]
  00000001421B1548: mov         rax,qword ptr [rbp+3E8h]
  00000001421B154F: movsd       xmm6,mmword ptr [rax+8]
  00000001421B1554: lea         rcx,[rbp+338h]
  00000001421B155B: movaps      xmm1,xmm10
  00000001421B155F: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001421B1564: mov         ecx,eax
  00000001421B1566: cmp         rdi,rcx
  00000001421B1569: jbe         00000001421B22FF
  00000001421B156F: lea         rax,[rcx+1]
  00000001421B1573: cmp         rax,rdi
  00000001421B1576: movss       xmm2,dword ptr [rbp+240h]
  00000001421B157E: jae         00000001421B22E8
  00000001421B1584: movss       xmm5,dword ptr [__real@3f800000]
  00000001421B158C: movaps      xmm1,xmm5
  00000001421B158F: subss       xmm1,xmm11
  00000001421B1594: movsldup    xmm1,xmm1
  00000001421B1598: mulps       xmm1,xmm8
  00000001421B159C: movsldup    xmm8,xmm11
  00000001421B15A1: mulps       xmm8,xmm7
  00000001421B15A5: addps       xmm8,xmm1
  00000001421B15A9: unpcklps    xmm8,xmm8
  00000001421B15AD: movaps      xmm1,xmm5
  00000001421B15B0: subss       xmm1,xmm0
  00000001421B15B4: movsd       xmm3,mmword ptr [r14+rcx*8]
  00000001421B15BA: movsd       xmm4,mmword ptr [r14+rcx*8+8]
  00000001421B15C1: movsldup    xmm1,xmm1
  00000001421B15C5: mulps       xmm1,xmm3
  00000001421B15C8: movsldup    xmm0,xmm0
  00000001421B15CC: mulps       xmm0,xmm4
  00000001421B15CF: addps       xmm0,xmm1
  00000001421B15D2: unpcklps    xmm0,xmm0
  00000001421B15D5: movaps      xmm3,xmmword ptr [rbp+260h]
  00000001421B15DC: mulps       xmm8,xmm3
  00000001421B15E0: movaps      xmm1,xmm8
  00000001421B15E4: unpckhpd    xmm1,xmm8
  00000001421B15E9: addps       xmm1,xmm8
  00000001421B15ED: addps       xmm6,xmm1
  00000001421B15F0: mulps       xmm0,xmm3
  00000001421B15F3: movaps      xmm1,xmm0
  00000001421B15F6: unpckhpd    xmm1,xmm0
  00000001421B15FA: addps       xmm1,xmm0
  00000001421B15FD: mov         rax,qword ptr [rbp+3E8h]
  00000001421B1604: movsd       xmm0,mmword ptr [rax+8]
  00000001421B1609: addps       xmm0,xmm1
  00000001421B160C: movaps      xmm1,xmm6
  00000001421B160F: subps       xmm1,xmm0
  00000001421B1612: movaps      xmm4,xmm1
  00000001421B1615: mulps       xmm4,xmm1
  00000001421B1618: movshdup    xmm3,xmm4
  00000001421B161C: addss       xmm3,xmm4
  00000001421B1620: xorps       xmm4,xmm4
  00000001421B1623: sqrtss      xmm4,xmm3
  00000001421B1627: movaps      xmm3,xmm5
  00000001421B162A: movss       dword ptr [rbp+2F0h],xmm4
  00000001421B1632: divss       xmm3,xmm4
  00000001421B1636: movsldup    xmm3,xmm3
  00000001421B163A: mulps       xmm3,xmm1
  00000001421B163D: movlps      qword ptr [rbp+0E0h],xmm3
  00000001421B1644: movshdup    xmm1,xmm3
  00000001421B1648: xorps       xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B164F: movss       dword ptr [rbp+3D8h],xmm1
  00000001421B1657: movss       dword ptr [rbp+3DCh],xmm3
  00000001421B165F: addps       xmm0,xmm6
  00000001421B1662: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B1669: movlps      qword ptr [rbp+328h],xmm0
  00000001421B1670: lea         rcx,[rbp+230h]
  00000001421B1677: lea         rdx,[rbp+328h]
  00000001421B167E: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421B1683: lea         rcx,[rbp+3A8h]
  00000001421B168A: call        _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E
  00000001421B168F: movaps      xmm11,xmm0
  00000001421B1693: movss       xmm6,dword ptr [__real@3f800000]
  00000001421B169B: subss       xmm6,xmm0
  00000001421B169F: mulss       xmm6,dword ptr [__real@3f4ccccd]
  00000001421B16A7: mulss       xmm11,dword ptr [__real@3f99999a]
  00000001421B16B0: lea         rcx,[rbp+2B8h]
  00000001421B16B7: movss       xmm1,dword ptr [rbp+2A0h]
  00000001421B16BF: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001421B16C4: mov         eax,eax
  00000001421B16C6: lea         rcx,[rax+1]
  00000001421B16CA: mov         rdx,qword ptr [rbp+2C8h]
  00000001421B16D1: cmp         rcx,rdx
  00000001421B16D4: jae         00000001421B2313
  00000001421B16DA: mov         rcx,qword ptr [rbp+2C0h]
  00000001421B16E1: movss       xmm1,dword ptr [rcx+rax*8+8]
  00000001421B16E7: movss       xmm2,dword ptr [rcx+rax*8+0Ch]
  00000001421B16ED: subss       xmm1,dword ptr [rcx+rax*8]
  00000001421B16F2: subss       xmm2,dword ptr [rcx+rax*8+4]
  00000001421B16F8: movaps      xmm0,xmm1
  00000001421B16FB: mulss       xmm0,xmm1
  00000001421B16FF: movaps      xmm3,xmm2
  00000001421B1702: mulss       xmm3,xmm2
  00000001421B1706: addss       xmm3,xmm0
  00000001421B170A: sqrtss      xmm3,xmm3
  00000001421B170E: movss       xmm0,dword ptr [__real@3f800000]
  00000001421B1716: divss       xmm0,xmm3
  00000001421B171A: mulss       xmm1,xmm0
  00000001421B171E: mulss       xmm0,xmm2
  00000001421B1722: divss       xmm0,xmm1
  00000001421B1726: call        atanf
  00000001421B172B: movaps      xmm12,xmm0
  00000001421B172F: lea         rcx,[rbp+0D0h]
  00000001421B1736: lea         rdx,[rbp+0E0h]
  00000001421B173D: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421B1742: movsd       xmm7,mmword ptr [rbp+0D0h]
  00000001421B174A: movss       xmm15,dword ptr [rbp+0D8h]
  00000001421B1753: lea         rcx,[rbp+430h]
  00000001421B175A: lea         rdx,[rbp+3D8h]
  00000001421B1761: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001421B1766: movss       dword ptr [rbp+390h],xmm10
  00000001421B176F: movss       xmm10,dword ptr [__real@3f000000]
  00000001421B1778: mulss       xmm12,xmm10
  00000001421B177D: movaps      xmm0,xmm12
  00000001421B1781: call        sinf
  00000001421B1786: movaps      xmm13,xmm0
  00000001421B178A: movsldup    xmm14,xmm0
  00000001421B178F: mulps       xmm14,xmm7
  00000001421B1793: mulss       xmm13,xmm15
  00000001421B1798: movaps      xmm0,xmm13
  00000001421B179C: shufps      xmm0,xmm14,30h
  00000001421B17A1: movaps      xmm8,xmm14
  00000001421B17A5: shufps      xmm8,xmm0,84h
  00000001421B17AA: movaps      xmm0,xmm12
  00000001421B17AE: call        cosf
  00000001421B17B3: movlhps     xmm0,xmm13
  00000001421B17B7: shufps      xmm14,xmm0,24h
  00000001421B17BC: movsd       xmm1,mmword ptr [rbp+434h]
  00000001421B17C4: movss       xmm3,dword ptr [rbp+430h]
  00000001421B17CC: movlhps     xmm3,xmm1
  00000001421B17CF: movaps      xmm0,xmm3
  00000001421B17D2: shufps      xmm0,xmm1,0D8h
  00000001421B17D6: shufps      xmm3,xmm1,58h
  00000001421B17DA: movaps      xmm2,xmm14
  00000001421B17DE: mulps       xmm2,xmm14
  00000001421B17E2: movshdup    xmm1,xmm2
  00000001421B17E6: addss       xmm1,xmm2
  00000001421B17EA: movaps      xmm4,xmm2
  00000001421B17ED: unpckhpd    xmm4,xmm2
  00000001421B17F1: addss       xmm4,xmm1
  00000001421B17F5: shufps      xmm4,xmm4,0
  00000001421B17F9: shufps      xmm2,xmm2,0FFh
  00000001421B17FD: subps       xmm2,xmm4
  00000001421B1800: mulps       xmm2,xmm3
  00000001421B1803: movaps      xmm1,xmm8
  00000001421B1807: mulps       xmm1,xmm0
  00000001421B180A: movshdup    xmm3,xmm1
  00000001421B180E: addss       xmm3,xmm1
  00000001421B1812: movhlps     xmm1,xmm1
  00000001421B1815: addss       xmm1,xmm3
  00000001421B1819: addss       xmm1,xmm1
  00000001421B181D: shufps      xmm1,xmm1,0
  00000001421B1821: mulps       xmm1,xmm14
  00000001421B1825: addps       xmm1,xmm2
  00000001421B1828: movaps      xmm2,xmm8
  00000001421B182C: shufps      xmm2,xmm8,0D2h
  00000001421B1831: mulps       xmm2,xmm0
  00000001421B1834: shufps      xmm0,xmm0,0D2h
  00000001421B1838: mulps       xmm0,xmm8
  00000001421B183C: subps       xmm2,xmm0
  00000001421B183F: shufps      xmm2,xmm2,0D2h
  00000001421B1843: addps       xmm14,xmm14
  00000001421B1847: shufps      xmm14,xmm14,0FFh
  00000001421B184C: mulps       xmm14,xmm2
  00000001421B1850: addps       xmm14,xmm1
  00000001421B1854: movshdup    xmm4,xmm14
  00000001421B1859: movaps      xmm0,xmm15
  00000001421B185D: mulss       xmm0,xmm4
  00000001421B1861: movaps      xmm13,xmm14
  00000001421B1865: unpckhpd    xmm13,xmm14
  00000001421B186A: movshdup    xmm12,xmm7
  00000001421B186F: movaps      xmm1,xmm12
  00000001421B1873: mulss       xmm1,xmm13
  00000001421B1878: subss       xmm0,xmm1
  00000001421B187C: movaps      xmm1,xmm7
  00000001421B187F: mulss       xmm1,xmm13
  00000001421B1884: movaps      xmm2,xmm15
  00000001421B1888: mulss       xmm2,xmm14
  00000001421B188D: subss       xmm1,xmm2
  00000001421B1891: movaps      xmm2,xmm12
  00000001421B1895: mulss       xmm2,xmm14
  00000001421B189A: movaps      xmm3,xmm4
  00000001421B189D: mulss       xmm3,xmm7
  00000001421B18A1: subss       xmm2,xmm3
  00000001421B18A5: movaps      xmm3,xmm13
  00000001421B18A9: xor         edi,edi
  00000001421B18AB: ucomiss     xmm9,xmm10
  00000001421B18AF: movaps      xmm8,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B18B7: xorps       xmm3,xmm8
  00000001421B18BB: seta        dil
  00000001421B18BF: shl         edi,4
  00000001421B18C2: movups      xmm5,xmmword ptr [rbp+rdi+280h]
  00000001421B18CA: movaps      xmmword ptr [rbp-20h],xmm5
  00000001421B18CE: xorps       xmm10,xmm10
  00000001421B18D2: ucomiss     xmm10,xmm2
  00000001421B18D6: jae         00000001421B1940
  00000001421B18D8: movaps      xmm8,xmm7
  00000001421B18DC: subss       xmm8,xmm4
  00000001421B18E1: addss       xmm2,dword ptr [__real@3f800000]
  00000001421B18E9: ucomiss     xmm10,xmm8
  00000001421B18ED: jae         00000001421B19B0
  00000001421B18F3: addss       xmm8,xmm2
  00000001421B18F8: xorps       xmm2,xmm2
  00000001421B18FB: sqrtss      xmm2,xmm8
  00000001421B1900: movss       xmm5,dword ptr [__real@3f000000]
  00000001421B1908: divss       xmm5,xmm2
  00000001421B190C: unpcklps    xmm3,xmm0
  00000001421B190F: unpcklps    xmm1,xmm15
  00000001421B1913: subps       xmm3,xmm1
  00000001421B1916: movsldup    xmm4,xmm5
  00000001421B191A: mulps       xmm4,xmm3
  00000001421B191D: movaps      xmm2,xmm12
  00000001421B1921: addss       xmm2,xmm14
  00000001421B1926: mulss       xmm2,xmm5
  00000001421B192A: mulss       xmm5,xmm8
  00000001421B192F: jmp         00000001421B1A44
  00000001421B1934: nop         word ptr cs:[rax+rax]
  00000001421B1940: xorps       xmm4,xmm8
  00000001421B1944: subss       xmm4,xmm7
  00000001421B1948: movss       xmm5,dword ptr [__real@3f800000]
  00000001421B1950: subss       xmm5,xmm2
  00000001421B1954: ucomiss     xmm10,xmm4
  00000001421B1958: jae         00000001421B1A00
  00000001421B195E: addss       xmm4,xmm5
  00000001421B1962: xorps       xmm2,xmm2
  00000001421B1965: sqrtss      xmm2,xmm4
  00000001421B1969: movss       xmm3,dword ptr [__real@3f000000]
  00000001421B1971: divss       xmm3,xmm2
  00000001421B1975: movaps      xmm2,xmm12
  00000001421B1979: subss       xmm2,xmm14
  00000001421B197E: unpcklps    xmm2,xmm4
  00000001421B1981: movsldup    xmm4,xmm3
  00000001421B1985: mulps       xmm4,xmm2
  00000001421B1988: subss       xmm1,xmm13
  00000001421B198D: mulss       xmm1,xmm3
  00000001421B1991: subss       xmm0,xmm15
  00000001421B1996: mulss       xmm0,xmm3
  00000001421B199A: movaps      xmm2,xmm1
  00000001421B199D: movaps      xmm5,xmm0
  00000001421B19A0: jmp         00000001421B1A44
  00000001421B19A5: nop         word ptr cs:[rax+rax]
  00000001421B19B0: subss       xmm2,xmm8
  00000001421B19B5: xorps       xmm3,xmm3
  00000001421B19B8: sqrtss      xmm3,xmm2
  00000001421B19BC: movss       xmm8,dword ptr [__real@3f000000]
  00000001421B19C5: divss       xmm8,xmm3
  00000001421B19CA: addss       xmm0,xmm15
  00000001421B19CF: subss       xmm1,xmm13
  00000001421B19D4: unpcklps    xmm0,xmm1
  00000001421B19D7: movsldup    xmm4,xmm8
  00000001421B19DC: mulps       xmm4,xmm0
  00000001421B19DF: mulss       xmm2,xmm8
  00000001421B19E4: movaps      xmm5,xmm12
  00000001421B19E8: addss       xmm5,xmm14
  00000001421B19ED: mulss       xmm5,xmm8
  00000001421B19F2: jmp         00000001421B1A44
  00000001421B19F4: nop         word ptr cs:[rax+rax]
  00000001421B1A00: movaps      xmm2,xmm7
  00000001421B1A03: movss       xmm2,xmm5
  00000001421B1A07: movsldup    xmm5,xmm14
  00000001421B1A0C: movss       xmm5,xmm4
  00000001421B1A10: subps       xmm2,xmm5
  00000001421B1A13: xorps       xmm4,xmm4
  00000001421B1A16: sqrtss      xmm4,xmm2
  00000001421B1A1A: movss       xmm5,dword ptr [__real@3f000000]
  00000001421B1A22: divss       xmm5,xmm4
  00000001421B1A26: movsldup    xmm4,xmm5
  00000001421B1A2A: mulps       xmm4,xmm2
  00000001421B1A2D: addss       xmm0,xmm15
  00000001421B1A32: mulss       xmm0,xmm5
  00000001421B1A36: subss       xmm3,xmm1
  00000001421B1A3A: mulss       xmm3,xmm5
  00000001421B1A3E: movaps      xmm2,xmm0
  00000001421B1A41: movaps      xmm5,xmm3
  00000001421B1A44: movss       xmm8,dword ptr [rbp+390h]
  00000001421B1A4D: movlhps     xmm5,xmm2
  00000001421B1A50: shufps      xmm4,xmm5,24h
  00000001421B1A54: movaps      xmmword ptr [rbp-30h],xmm4
  00000001421B1A58: lea         rcx,[rbp-30h]
  00000001421B1A5C: call        _ZN69_$LT$glam..f32..sse2..quat..Quat$u20$as$u20$utils..halver..Halver$GT$6to_f1617h7d373b4258af3426E
  00000001421B1A61: mov         r14,rax
  00000001421B1A64: lea         rax,[rdi+rbp]
  00000001421B1A68: add         rax,280h
  00000001421B1A6E: movups      xmm0,xmmword ptr [rax]
  00000001421B1A71: movaps      xmmword ptr [rbp+430h],xmm0
  00000001421B1A78: lea         rcx,[rbp+430h]
  00000001421B1A7F: call        _ZN66_$LT$$u5b$f32$u3b$$u20$4$u5d$$u20$as$u20$utils..halver..Halver$GT$6to_f1617h16803eaa118aabc6E
  00000001421B1A84: mov         rdi,rax
  00000001421B1A87: addss       xmm11,xmm6
  00000001421B1A8C: mulss       xmm11,dword ptr [rbp+380h]
  00000001421B1A95: movss       xmm0,dword ptr [rbp+2F0h]
  00000001421B1A9D: movss       dword ptr [rbp+430h],xmm0
  00000001421B1AA5: movss       dword ptr [rbp+434h],xmm11
  00000001421B1AAE: mov         dword ptr [rbp+438h],3DCCCCCDh
  00000001421B1AB8: lea         rcx,[rbp+430h]
  00000001421B1ABF: call        _ZN63_$LT$glam..f32..vec3..Vec3$u20$as$u20$utils..halver..Halver$GT$6to_f1617h51c93e75a0ff6032E
  00000001421B1AC4: mov         r15,rax
  00000001421B1AC7: movss       xmm6,dword ptr [rbp+3A4h]
  00000001421B1ACF: lea         rcx,[rbp+3A8h]
  00000001421B1AD6: call        _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E
  00000001421B1ADB: addss       xmm6,dword ptr [__real@bf800000]
  00000001421B1AE3: movss       xmm1,dword ptr [rbp+300h]
  00000001421B1AEB: divss       xmm1,xmm6
  00000001421B1AEF: mulss       xmm0,dword ptr [__real@3e4ccccd]
  00000001421B1AF7: addss       xmm0,xmm1
  00000001421B1AFB: xorps       xmm1,xmm1
  00000001421B1AFE: maxss       xmm1,xmm0
  00000001421B1B02: movss       xmm0,dword ptr [__real@3f800000]
  00000001421B1B0A: minss       xmm0,xmm1
  00000001421B1B0E: call        _ZN45_$LT$f32$u20$as$u20$utils..halver..Halver$GT$6to_f1617h628d79fa31310e7aE
  00000001421B1B13: mov         ecx,2
  00000001421B1B18: test        sil,sil
  00000001421B1B1B: jne         00000001421B1B65
  00000001421B1B1D: test        byte ptr [rbp+120h],1
  00000001421B1B24: je          00000001421B1B60
  00000001421B1B26: movss       xmm2,dword ptr [rbp+130h]
  00000001421B1B2E: mulss       xmm9,xmm2
  00000001421B1B33: movss       xmm1,dword ptr [__real@3f000000]
  00000001421B1B3B: ucomiss     xmm1,xmm9
  00000001421B1B3F: ja          00000001421B1B65
  00000001421B1B41: movss       xmm0,dword ptr [__real@3f800000]
  00000001421B1B49: subss       xmm0,xmm8
  00000001421B1B4E: mulss       xmm0,xmm2
  00000001421B1B52: ucomiss     xmm1,xmm0
  00000001421B1B55: ja          00000001421B1B65
  00000001421B1B57: nop         word ptr [rax+rax]
  00000001421B1B60: mov         ecx,1
  00000001421B1B65: mov         edx,dword ptr [rbp+238h]
  00000001421B1B6B: mov         dword ptr [rbp+438h],edx
  00000001421B1B71: mov         rdx,qword ptr [rbp+230h]
  00000001421B1B78: mov         qword ptr [rbp+430h],rdx
  00000001421B1B7F: mov         dword ptr [rbp+43Ch],0
  00000001421B1B89: mov         qword ptr [rbp+440h],rdi
  00000001421B1B90: mov         dword ptr [rbp+448h],r15d
  00000001421B1B97: shr         r15,20h
  00000001421B1B9B: mov         word ptr [rbp+44Ch],r15w
  00000001421B1BA3: mov         word ptr [rbp+44Eh],ax
  00000001421B1BAA: mov         qword ptr [rbp+450h],r14
  00000001421B1BB1: mov         eax,dword ptr [rbp+404h]
  00000001421B1BB7: mov         dword ptr [rbp+458h],eax
  00000001421B1BBD: mov         qword ptr [rbp+45Ch],0
  00000001421B1BC8: mov         eax,dword ptr [rbp+474h]
  00000001421B1BCE: mov         dword ptr [rbp+464h],eax
  00000001421B1BD4: mov         dword ptr [rbp+468h],0
  00000001421B1BDE: mov         dword ptr [rbp+46Ch],ecx
  00000001421B1BE4: mov         rcx,qword ptr [rbp+5A0h]
  00000001421B1BEB: lea         rdx,[rbp+430h]
  00000001421B1BF2: call        00000001421B2CA0
  00000001421B1BF7: cmp         qword ptr [rbp+250h],0
  00000001421B1BFF: jne         00000001421B1460
  00000001421B1C05: movsldup    xmm0,xmm11
  00000001421B1C0A: mulps       xmm0,xmm14
  00000001421B1C0E: mulss       xmm13,xmm11
  00000001421B1C13: mulss       xmm13,dword ptr [__real@3f000000]
  00000001421B1C1C: movss       xmm6,dword ptr [rbp+238h]
  00000001421B1C24: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B1C2B: movsd       xmm1,mmword ptr [rbp+230h]
  00000001421B1C33: movaps      xmm8,xmm1
  00000001421B1C37: subps       xmm8,xmm0
  00000001421B1C3B: movshdup    xmm2,xmm8
  00000001421B1C40: addss       xmm0,xmm1
  00000001421B1C44: movaps      xmm1,xmm13
  00000001421B1C48: addss       xmm1,xmm6
  00000001421B1C4C: movss       dword ptr [rbp+430h],xmm0
  00000001421B1C54: movss       dword ptr [rbp+434h],xmm1
  00000001421B1C5C: lea         rcx,[rbp+0C0h]
  00000001421B1C63: lea         rdx,[rbp+430h]
  00000001421B1C6A: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421B1C6F: subss       xmm6,xmm13
  00000001421B1C74: movsd       xmm10,mmword ptr [rbp+0C0h]
  00000001421B1C7D: movss       xmm9,dword ptr [rbp+0C8h]
  00000001421B1C86: movaps      xmm0,xmm9
  00000001421B1C8A: subss       xmm0,xmm6
  00000001421B1C8E: movaps      xmm1,xmm10
  00000001421B1C92: subps       xmm1,xmm8
  00000001421B1C96: mulps       xmm1,xmm1
  00000001421B1C99: movshdup    xmm2,xmm1
  00000001421B1C9D: addss       xmm2,xmm1
  00000001421B1CA1: mulss       xmm0,xmm0
  00000001421B1CA5: addss       xmm0,xmm2
  00000001421B1CA9: xorps       xmm1,xmm1
  00000001421B1CAC: sqrtss      xmm1,xmm0
  00000001421B1CB0: ucomiss     xmm1,dword ptr [__real@3dcccccd]
  00000001421B1CB7: jbe         00000001421B1460
  00000001421B1CBD: movaps      xmm3,xmm12
  00000001421B1CC1: xorps       xmm5,xmm5
  00000001421B1CC4: mulss       xmm3,xmm5
  00000001421B1CC8: movaps      xmm0,xmm3
  00000001421B1CCB: subss       xmm0,xmm7
  00000001421B1CCF: movaps      xmm4,xmm7
  00000001421B1CD2: mulss       xmm4,xmm5
  00000001421B1CD6: movaps      xmm2,xmm15
  00000001421B1CDA: mulss       xmm2,xmm5
  00000001421B1CDE: subss       xmm4,xmm2
  00000001421B1CE2: movss       xmm2,dword ptr [rbp+2F0h]
  00000001421B1CEA: movss       dword ptr [rbp+1CCh],xmm2
  00000001421B1CF2: movss       dword ptr [rbp+1D0h],xmm1
  00000001421B1CFA: mov         dword ptr [rbp+1D4h],3DCCCCCDh
  00000001421B1D04: movaps      xmm2,xmm15
  00000001421B1D08: unpcklps    xmm2,xmm4
  00000001421B1D0B: unpcklps    xmm3,xmm7
  00000001421B1D0E: subps       xmm2,xmm3
  00000001421B1D11: movshdup    xmm3,xmm2
  00000001421B1D15: ucomiss     xmm5,xmm3
  00000001421B1D18: jae         00000001421B1D67
  00000001421B1D1A: movaps      xmm1,xmm7
  00000001421B1D1D: shufps      xmm1,xmmword ptr [__xmm@00000000000000003f80000000000000],11h
  00000001421B1D25: shufps      xmm1,xmm7,0E8h
  00000001421B1D29: addps       xmm1,xmm2
  00000001421B1D2C: movshdup    xmm2,xmm1
  00000001421B1D30: sqrtss      xmm2,xmm2
  00000001421B1D34: movss       xmm3,dword ptr [__real@3f000000]
  00000001421B1D3C: divss       xmm3,xmm2
  00000001421B1D40: movsldup    xmm3,xmm3
  00000001421B1D44: mulps       xmm1,xmm3
  00000001421B1D47: movsldup    xmm4,xmm15
  00000001421B1D4C: xorps       xmm2,xmm2
  00000001421B1D4F: subps       xmm2,xmm4
  00000001421B1D52: addss       xmm0,dword ptr [__real@3f800000]
  00000001421B1D5A: shufps      xmm2,xmm0,1
  00000001421B1D5E: shufps      xmm2,xmm0,0E2h
  00000001421B1D62: mulps       xmm2,xmm3
  00000001421B1D65: jmp         00000001421B1DAB
  00000001421B1D67: movss       xmm1,dword ptr [__real@3f800000]
  00000001421B1D6F: subss       xmm1,xmm3
  00000001421B1D73: xorps       xmm3,xmm3
  00000001421B1D76: sqrtss      xmm3,xmm1
  00000001421B1D7A: movss       xmm4,dword ptr [__real@3f000000]
  00000001421B1D82: divss       xmm4,xmm3
  00000001421B1D86: addss       xmm2,xmm12
  00000001421B1D8B: unpcklps    xmm1,xmm2
  00000001421B1D8E: movsldup    xmm2,xmm4
  00000001421B1D92: mulps       xmm1,xmm2
  00000001421B1D95: movlhps     xmm0,xmm15
  00000001421B1D99: shufps      xmm0,xmm15,0E2h
  00000001421B1D9E: addps       xmm0,xmmword ptr [__xmm@0000000000000000bf80000000000000]
  00000001421B1DA5: mulps       xmm0,xmm2
  00000001421B1DA8: movaps      xmm2,xmm0
  00000001421B1DAB: movlhps     xmm1,xmm2
  00000001421B1DAE: movaps      xmmword ptr [rbp-40h],xmm1
  00000001421B1DB2: lea         rcx,[rbp-40h]
  00000001421B1DB6: call        _ZN69_$LT$glam..f32..sse2..quat..Quat$u20$as$u20$utils..halver..Halver$GT$6to_f1617h7d373b4258af3426E
  00000001421B1DBB: mov         rdi,rax
  00000001421B1DBE: lea         rcx,[rbp-20h]
  00000001421B1DC2: call        _ZN66_$LT$$u5b$f32$u3b$$u20$4$u5d$$u20$as$u20$utils..halver..Halver$GT$6to_f1617h16803eaa118aabc6E
  00000001421B1DC7: mov         r14,rax
  00000001421B1DCA: lea         rcx,[rbp+1CCh]
  00000001421B1DD1: call        _ZN63_$LT$glam..f32..vec3..Vec3$u20$as$u20$utils..halver..Halver$GT$6to_f1617h51c93e75a0ff6032E
  00000001421B1DD6: mov         r15,rax
  00000001421B1DD9: xorps       xmm0,xmm0
  00000001421B1DDC: call        _ZN45_$LT$f32$u20$as$u20$utils..halver..Halver$GT$6to_f1617h628d79fa31310e7aE
  00000001421B1DE1: movaps      xmm1,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001421B1DE8: mulps       xmm10,xmm1
  00000001421B1DEC: movss       xmm0,dword ptr [__real@3f000000]
  00000001421B1DF4: mulss       xmm9,xmm0
  00000001421B1DF9: mulps       xmm8,xmm1
  00000001421B1DFD: addps       xmm8,xmm10
  00000001421B1E01: mulss       xmm6,xmm0
  00000001421B1E05: addss       xmm6,xmm9
  00000001421B1E0A: movlps      qword ptr [rbp+430h],xmm8
  00000001421B1E12: movss       dword ptr [rbp+438h],xmm6
  00000001421B1E1A: mov         dword ptr [rbp+43Ch],0
  00000001421B1E24: mov         qword ptr [rbp+440h],r14
  00000001421B1E2B: mov         dword ptr [rbp+448h],r15d
  00000001421B1E32: shr         r15,20h
  00000001421B1E36: mov         word ptr [rbp+44Ch],r15w
  00000001421B1E3E: mov         word ptr [rbp+44Eh],ax
  00000001421B1E45: mov         qword ptr [rbp+450h],rdi
  00000001421B1E4C: mov         eax,dword ptr [rbp+404h]
  00000001421B1E52: mov         dword ptr [rbp+458h],eax
  00000001421B1E58: mov         qword ptr [rbp+45Ch],0
  00000001421B1E63: mov         eax,dword ptr [rbp+474h]
  00000001421B1E69: mov         dword ptr [rbp+464h],eax
  00000001421B1E6F: mov         rax,200000000h
  00000001421B1E79: mov         qword ptr [rbp+468h],rax
  00000001421B1E80: mov         rcx,qword ptr [rbp+5A0h]
  00000001421B1E87: lea         rdx,[rbp+430h]
  00000001421B1E8E: call        00000001421B2CA0
  00000001421B1E93: jmp         00000001421B1460
  00000001421B1E98: nop         dword ptr [rax+rax]
  00000001421B1EA0: mov         rdx,qword ptr [rbp+338h]
  00000001421B1EA7: test        rdx,rdx
  00000001421B1EAA: je          00000001421B1EC2
  00000001421B1EAC: mov         rcx,qword ptr [rbp+340h]
  00000001421B1EB3: shl         rdx,3
  00000001421B1EB7: mov         r8d,4
  00000001421B1EBD: call        __rust_dealloc
  00000001421B1EC2: mov         rdx,qword ptr [rbp+350h]
  00000001421B1EC9: test        rdx,rdx
  00000001421B1ECC: mov         r12,qword ptr [rbp+3B0h]
  00000001421B1ED3: mov         rdi,qword ptr [rbp+370h]
  00000001421B1EDA: mov         rbx,qword ptr [rbp+228h]
  00000001421B1EE1: mov         r14,qword ptr [rbp+218h]
  00000001421B1EE8: mov         r15,qword ptr [rbp+220h]
  00000001421B1EEF: mov         r13,qword ptr [rbp+410h]
  00000001421B1EF6: mov         rsi,qword ptr [rbp+160h]
  00000001421B1EFD: je          00000001421B1F15
  00000001421B1EFF: mov         rcx,qword ptr [rbp+358h]
  00000001421B1F06: shl         rdx,2
  00000001421B1F0A: mov         r8d,4
  00000001421B1F10: call        __rust_dealloc
  00000001421B1F15: mov         qword ptr [rbp+410h],r13
  00000001421B1F1C: mov         rdx,qword ptr [rbp+1F0h]
  00000001421B1F23: test        rdx,rdx
  00000001421B1F26: je          00000001421B10D0
  00000001421B1F2C: mov         rcx,qword ptr [rbp+1F8h]
  00000001421B1F33: shl         rdx,2
  00000001421B1F37: mov         r8d,4
  00000001421B1F3D: call        __rust_dealloc
  00000001421B1F42: jmp         00000001421B10D0
  00000001421B1F47: mov         rax,qword ptr [rbp+278h]
  00000001421B1F4E: mov         qword ptr [rbp+210h],rax
  00000001421B1F55: mov         rax,qword ptr [rbp+310h]
  00000001421B1F5C: test        rax,rax
  00000001421B1F5F: mov         r13,r12
  00000001421B1F62: je          00000001421B1F7E
  00000001421B1F64: mov         rcx,qword ptr [rbp+318h]
  00000001421B1F6B: shl         rax,3
  00000001421B1F6F: lea         rdx,[rax+rax*2]
  00000001421B1F73: mov         r8d,4
  00000001421B1F79: call        __rust_dealloc
  00000001421B1F7E: mov         rdx,qword ptr [rbp+2B8h]
  00000001421B1F85: test        rdx,rdx
  00000001421B1F88: je          00000001421B1FA0
  00000001421B1F8A: mov         rcx,qword ptr [rbp+2C0h]
  00000001421B1F91: shl         rdx,3
  00000001421B1F95: mov         r8d,4
  00000001421B1F9B: call        __rust_dealloc
  00000001421B1FA0: mov         rdx,qword ptr [rbp+2D0h]
  00000001421B1FA7: test        rdx,rdx
  00000001421B1FAA: je          00000001421B1FC2
  00000001421B1FAC: mov         rcx,qword ptr [rbp+2D8h]
  00000001421B1FB3: shl         rdx,2
  00000001421B1FB7: mov         r8d,4
  00000001421B1FBD: call        __rust_dealloc
  00000001421B1FC2: mov         r11,qword ptr [rbp+0F8h]
  00000001421B1FC9: movaps      xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421B1FD0: mov         rdx,qword ptr [rbp+3F0h]
  00000001421B1FD7: test        bl,1
  00000001421B1FDA: je          00000001421B06F9
  00000001421B1FE0: jmp         00000001421B09F0
  00000001421B1FE5: mov         rdx,qword ptr [rbp+8]
  00000001421B1FE9: test        rdx,rdx
  00000001421B1FEC: je          00000001421B2001
  00000001421B1FEE: mov         rcx,qword ptr [rbp+10h]
  00000001421B1FF2: shl         rdx,3
  00000001421B1FF6: mov         r8d,4
  00000001421B1FFC: call        __rust_dealloc
  00000001421B2001: mov         rdx,qword ptr [rbp+20h]
  00000001421B2005: test        rdx,rdx
  00000001421B2008: je          00000001421B201D
  00000001421B200A: mov         rcx,qword ptr [rbp+28h]
  00000001421B200E: shl         rdx,2
  00000001421B2012: mov         r8d,4
  00000001421B2018: call        __rust_dealloc
  00000001421B201D: mov         rdx,qword ptr [rbp+40h]
  00000001421B2021: test        rdx,rdx
  00000001421B2024: je          00000001421B2039
  00000001421B2026: mov         rcx,qword ptr [rbp+48h]
  00000001421B202A: shl         rdx,3
  00000001421B202E: mov         r8d,4
  00000001421B2034: call        __rust_dealloc
  00000001421B2039: mov         rdx,qword ptr [rbp+58h]
  00000001421B203D: test        rdx,rdx
  00000001421B2040: je          00000001421B2055
  00000001421B2042: mov         rcx,qword ptr [rbp+60h]
  00000001421B2046: shl         rdx,2
  00000001421B204A: mov         r8d,4
  00000001421B2050: call        __rust_dealloc
  00000001421B2055: mov         rdx,qword ptr [rbp+190h]
  00000001421B205C: test        rdx,rdx
  00000001421B205F: je          00000001421B2077
  00000001421B2061: mov         rcx,qword ptr [rbp+198h]
  00000001421B2068: shl         rdx,3
  00000001421B206C: mov         r8d,4
  00000001421B2072: call        __rust_dealloc
  00000001421B2077: mov         rdx,qword ptr [rbp+1A8h]
  00000001421B207E: test        rdx,rdx
  00000001421B2081: je          00000001421B2099
  00000001421B2083: mov         rcx,qword ptr [rbp+1B0h]
  00000001421B208A: shl         rdx,2
  00000001421B208E: mov         r8d,4
  00000001421B2094: call        __rust_dealloc
  00000001421B2099: movaps      xmm6,xmmword ptr [rbp+490h]
  00000001421B20A0: movaps      xmm7,xmmword ptr [rbp+4A0h]
  00000001421B20A7: movaps      xmm8,xmmword ptr [rbp+4B0h]
  00000001421B20AF: movaps      xmm9,xmmword ptr [rbp+4C0h]
  00000001421B20B7: movaps      xmm10,xmmword ptr [rbp+4D0h]
  00000001421B20BF: movaps      xmm11,xmmword ptr [rbp+4E0h]
  00000001421B20C7: movaps      xmm12,xmmword ptr [rbp+4F0h]
  00000001421B20CF: movaps      xmm13,xmmword ptr [rbp+500h]
  00000001421B20D7: movaps      xmm14,xmmword ptr [rbp+510h]
  00000001421B20DF: movaps      xmm15,xmmword ptr [rbp+520h]
  00000001421B20E7: add         rsp,5B8h
  00000001421B20EE: pop         rbx
  00000001421B20EF: pop         rdi
  00000001421B20F0: pop         rsi
  00000001421B20F1: pop         r12
  00000001421B20F3: pop         r13
  00000001421B20F5: pop         r14
  00000001421B20F7: pop         r15
  00000001421B20F9: pop         rbp
  00000001421B20FA: ret
  00000001421B20FB: lea         rax,[rbp+3D8h]
  00000001421B2102: mov         qword ptr [rbp+328h],rax
  00000001421B2109: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  00000001421B2110: mov         qword ptr [rbp+330h],rax
  00000001421B2117: lea         rax,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.1.llvm.17463645933293978377]
  00000001421B211E: mov         qword ptr [rbp+430h],rax
  00000001421B2125: mov         qword ptr [rbp+438h],1
  00000001421B2130: mov         qword ptr [rbp+450h],0
  00000001421B213B: lea         rax,[rbp+328h]
  00000001421B2142: mov         qword ptr [rbp+440h],rax
  00000001421B2149: mov         qword ptr [rbp+448h],1
  00000001421B2154: lea         rdx,[142FF8288h]
  00000001421B215B: lea         rcx,[rbp+430h]
  00000001421B2162: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  00000001421B2167: jmp         00000001421B23A9
  00000001421B216C: lea         rcx,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.2.llvm.17463645933293978377]
  00000001421B2173: lea         r8,[142FF8288h]
  00000001421B217A: mov         edx,1Dh
  00000001421B217F: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001421B2184: jmp         00000001421B23A9
  00000001421B2189: lea         rax,[rbp+3D8h]
  00000001421B2190: mov         qword ptr [rbp+328h],rax
  00000001421B2197: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  00000001421B219E: mov         qword ptr [rbp+330h],rax
  00000001421B21A5: lea         rax,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.1.llvm.17463645933293978377]
  00000001421B21AC: mov         qword ptr [rbp+430h],rax
  00000001421B21B3: mov         qword ptr [rbp+438h],1
  00000001421B21BE: mov         qword ptr [rbp+450h],0
  00000001421B21C9: lea         rax,[rbp+328h]
  00000001421B21D0: mov         qword ptr [rbp+440h],rax
  00000001421B21D7: mov         qword ptr [rbp+448h],1
  00000001421B21E2: lea         rdx,[142FF82B8h]
  00000001421B21E9: lea         rcx,[rbp+430h]
  00000001421B21F0: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  00000001421B21F5: jmp         00000001421B23A9
  00000001421B21FA: lea         rcx,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.2.llvm.17463645933293978377]
  00000001421B2201: lea         r8,[142FF82B8h]
  00000001421B2208: mov         edx,1Dh
  00000001421B220D: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001421B2212: jmp         00000001421B23A9
  00000001421B2217: lea         r8,[142FF8210h]
  00000001421B221E: mov         rcx,rbx
  00000001421B2221: mov         rdx,rsi
  00000001421B2224: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2229: jmp         00000001421B23A9
  00000001421B222E: lea         r8,[142FF8240h]
  00000001421B2235: mov         rcx,rbx
  00000001421B2238: mov         rdx,rsi
  00000001421B223B: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2240: jmp         00000001421B23A9
  00000001421B2245: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.14.llvm.9475246483403468623]
  00000001421B224C: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2251: jmp         00000001421B23A9
  00000001421B2256: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.14.llvm.9475246483403468623]
  00000001421B225D: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2262: jmp         00000001421B23A9
  00000001421B2267: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.15.llvm.9475246483403468623]
  00000001421B226E: mov         rcx,rax
  00000001421B2271: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2276: jmp         00000001421B23A9
  00000001421B227B: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.15.llvm.9475246483403468623]
  00000001421B2282: mov         rcx,rax
  00000001421B2285: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B228A: jmp         00000001421B23A9
  00000001421B228F: lea         r8,[142FF8270h]
  00000001421B2296: mov         rcx,r12
  00000001421B2299: mov         rdx,rbx
  00000001421B229C: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B22A1: jmp         00000001421B23A9
  00000001421B22A6: lea         r8,[142FF82A0h]
  00000001421B22AD: mov         rcx,r12
  00000001421B22B0: mov         rdx,rbx
  00000001421B22B3: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B22B8: jmp         00000001421B23A9
  00000001421B22BD: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.14.llvm.9475246483403468623]
  00000001421B22C4: mov         rdx,rdi
  00000001421B22C7: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B22CC: jmp         00000001421B23A9
  00000001421B22D1: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.15.llvm.9475246483403468623]
  00000001421B22D8: mov         rcx,rax
  00000001421B22DB: mov         rdx,rdi
  00000001421B22DE: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B22E3: jmp         00000001421B23A9
  00000001421B22E8: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.15.llvm.9475246483403468623]
  00000001421B22EF: mov         rcx,rax
  00000001421B22F2: mov         rdx,rdi
  00000001421B22F5: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B22FA: jmp         00000001421B23A9
  00000001421B22FF: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.14.llvm.9475246483403468623]
  00000001421B2306: mov         rdx,rdi
  00000001421B2309: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B230E: jmp         00000001421B23A9
  00000001421B2313: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.16.llvm.9475246483403468623]
  00000001421B231A: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B231F: jmp         00000001421B23A9
  00000001421B2324: mov         ecx,4
  00000001421B2329: mov         edx,10h
  00000001421B232E: call        _ZN5alloc5alloc18handle_alloc_error17h3e7daf9bcd04547aE
  00000001421B2333: jmp         00000001421B23A9
  00000001421B2335: lea         r8,[142FF81C8h]
  00000001421B233C: mov         edx,2
  00000001421B2341: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2346: jmp         00000001421B23A9
  00000001421B2348: lea         r8,[142FF81B0h]
  00000001421B234F: mov         rcx,r13
  00000001421B2352: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2357: jmp         00000001421B23A9
  00000001421B2359: lea         rcx,[142FF8168h]
  00000001421B2360: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  00000001421B2365: jmp         00000001421B23A9
  00000001421B2367: lea         r8,[142FF8108h]
  00000001421B236E: mov         rdx,rsi
  00000001421B2371: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2376: jmp         00000001421B23A9
  00000001421B2378: lea         r8,[142FF8120h]
  00000001421B237F: mov         rdx,rdi
  00000001421B2382: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2387: jmp         00000001421B23A9
  00000001421B2389: lea         r8,[142FF8138h]
  00000001421B2390: mov         rdx,rdi
  00000001421B2393: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B2398: jmp         00000001421B23A9
  00000001421B239A: lea         r8,[142FF8150h]
  00000001421B23A1: mov         rdx,rsi
  00000001421B23A4: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001421B23A9: ud2
  00000001421B23AB: nop         dword ptr [rax+rax]
  00000001421B23B0: mov         qword ptr [rsp+10h],rdx
  00000001421B23B5: push        rbp
  00000001421B23B6: push        r15
  00000001421B23B8: push        r14
  00000001421B23BA: push        r13
  00000001421B23BC: push        r12
  00000001421B23BE: push        rsi
  00000001421B23BF: push        rdi
  00000001421B23C0: push        rbx
  00000001421B23C1: sub         rsp,0D8h
  00000001421B23C8: lea         rbp,[rdx+80h]
  00000001421B23CF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B23D5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B23DB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B23E1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B23E7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B23ED: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B23F6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B23FF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B2408: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2410: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2418: mov         rdx,qword ptr [rbp+2B8h]
  00000001421B241F: test        rdx,rdx
  00000001421B2422: je          00000001421B243A
  00000001421B2424: shl         rdx,2
  00000001421B2428: mov         r8d,4
  00000001421B242E: mov         rcx,qword ptr [rbp+2A0h]
  00000001421B2435: call        __rust_dealloc
  00000001421B243A: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2442: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B244A: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2453: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B245C: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2465: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B246B: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2471: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2477: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B247D: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2483: add         rsp,0D8h
  00000001421B248A: pop         rbx
  00000001421B248B: pop         rdi
  00000001421B248C: pop         rsi
  00000001421B248D: pop         r12
  00000001421B248F: pop         r13
  00000001421B2491: pop         r14
  00000001421B2493: pop         r15
  00000001421B2495: pop         rbp
  00000001421B2496: ret
  00000001421B2497: nop         word ptr [rax+rax]
  00000001421B24A0: mov         qword ptr [rsp+10h],rdx
  00000001421B24A5: push        rbp
  00000001421B24A6: push        r15
  00000001421B24A8: push        r14
  00000001421B24AA: push        r13
  00000001421B24AC: push        r12
  00000001421B24AE: push        rsi
  00000001421B24AF: push        rdi
  00000001421B24B0: push        rbx
  00000001421B24B1: sub         rsp,0D8h
  00000001421B24B8: lea         rbp,[rdx+80h]
  00000001421B24BF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B24C5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B24CB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B24D1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B24D7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B24DD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B24E6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B24EF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B24F8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2500: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2508: mov         rdx,qword ptr [rbp+338h]
  00000001421B250F: test        rdx,rdx
  00000001421B2512: je          00000001421B252A
  00000001421B2514: mov         rcx,qword ptr [rbp+340h]
  00000001421B251B: shl         rdx,3
  00000001421B251F: mov         r8d,4
  00000001421B2525: call        __rust_dealloc
  00000001421B252A: mov         rdx,qword ptr [rbp+350h]
  00000001421B2531: test        rdx,rdx
  00000001421B2534: je          00000001421B254C
  00000001421B2536: mov         rcx,qword ptr [rbp+358h]
  00000001421B253D: shl         rdx,2
  00000001421B2541: mov         r8d,4
  00000001421B2547: call        __rust_dealloc
  00000001421B254C: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2554: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B255C: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2565: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B256E: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2577: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B257D: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2583: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2589: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B258F: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2595: add         rsp,0D8h
  00000001421B259C: pop         rbx
  00000001421B259D: pop         rdi
  00000001421B259E: pop         rsi
  00000001421B259F: pop         r12
  00000001421B25A1: pop         r13
  00000001421B25A3: pop         r14
  00000001421B25A5: pop         r15
  00000001421B25A7: pop         rbp
  00000001421B25A8: ret
  00000001421B25A9: nop         dword ptr [rax]
  00000001421B25B0: mov         qword ptr [rsp+10h],rdx
  00000001421B25B5: push        rbp
  00000001421B25B6: push        r15
  00000001421B25B8: push        r14
  00000001421B25BA: push        r13
  00000001421B25BC: push        r12
  00000001421B25BE: push        rsi
  00000001421B25BF: push        rdi
  00000001421B25C0: push        rbx
  00000001421B25C1: sub         rsp,0D8h
  00000001421B25C8: lea         rbp,[rdx+80h]
  00000001421B25CF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B25D5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B25DB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B25E1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B25E7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B25ED: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B25F6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B25FF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B2608: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2610: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2618: mov         rdx,qword ptr [rbp+1F0h]
  00000001421B261F: test        rdx,rdx
  00000001421B2622: je          00000001421B263A
  00000001421B2624: mov         rcx,qword ptr [rbp+1F8h]
  00000001421B262B: shl         rdx,2
  00000001421B262F: mov         r8d,4
  00000001421B2635: call        __rust_dealloc
  00000001421B263A: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2642: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B264A: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2653: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B265C: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2665: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B266B: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2671: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2677: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B267D: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2683: add         rsp,0D8h
  00000001421B268A: pop         rbx
  00000001421B268B: pop         rdi
  00000001421B268C: pop         rsi
  00000001421B268D: pop         r12
  00000001421B268F: pop         r13
  00000001421B2691: pop         r14
  00000001421B2693: pop         r15
  00000001421B2695: pop         rbp
  00000001421B2696: ret
  00000001421B2697: nop         word ptr [rax+rax]
  00000001421B26A0: mov         qword ptr [rsp+10h],rdx
  00000001421B26A5: push        rbp
  00000001421B26A6: push        r15
  00000001421B26A8: push        r14
  00000001421B26AA: push        r13
  00000001421B26AC: push        r12
  00000001421B26AE: push        rsi
  00000001421B26AF: push        rdi
  00000001421B26B0: push        rbx
  00000001421B26B1: sub         rsp,0D8h
  00000001421B26B8: lea         rbp,[rdx+80h]
  00000001421B26BF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B26C5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B26CB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B26D1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B26D7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B26DD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B26E6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B26EF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B26F8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2700: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2708: mov         rax,qword ptr [rbp+310h]
  00000001421B270F: test        rax,rax
  00000001421B2712: je          00000001421B272E
  00000001421B2714: mov         rcx,qword ptr [rbp+318h]
  00000001421B271B: shl         rax,3
  00000001421B271F: lea         rdx,[rax+rax*2]
  00000001421B2723: mov         r8d,4
  00000001421B2729: call        __rust_dealloc
  00000001421B272E: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2736: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B273E: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2747: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B2750: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2759: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B275F: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2765: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B276B: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B2771: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2777: add         rsp,0D8h
  00000001421B277E: pop         rbx
  00000001421B277F: pop         rdi
  00000001421B2780: pop         rsi
  00000001421B2781: pop         r12
  00000001421B2783: pop         r13
  00000001421B2785: pop         r14
  00000001421B2787: pop         r15
  00000001421B2789: pop         rbp
  00000001421B278A: ret
  00000001421B278B: nop         dword ptr [rax+rax]
  00000001421B2790: mov         qword ptr [rsp+10h],rdx
  00000001421B2795: push        rbp
  00000001421B2796: push        r15
  00000001421B2798: push        r14
  00000001421B279A: push        r13
  00000001421B279C: push        r12
  00000001421B279E: push        rsi
  00000001421B279F: push        rdi
  00000001421B27A0: push        rbx
  00000001421B27A1: sub         rsp,0D8h
  00000001421B27A8: lea         rbp,[rdx+80h]
  00000001421B27AF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B27B5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B27BB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B27C1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B27C7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B27CD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B27D6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B27DF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B27E8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B27F0: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B27F8: mov         rdx,qword ptr [rbp+2B8h]
  00000001421B27FF: test        rdx,rdx
  00000001421B2802: je          00000001421B281A
  00000001421B2804: mov         rcx,qword ptr [rbp+2C0h]
  00000001421B280B: shl         rdx,3
  00000001421B280F: mov         r8d,4
  00000001421B2815: call        __rust_dealloc
  00000001421B281A: mov         rdx,qword ptr [rbp+2D0h]
  00000001421B2821: test        rdx,rdx
  00000001421B2824: je          00000001421B283C
  00000001421B2826: mov         rcx,qword ptr [rbp+2D8h]
  00000001421B282D: shl         rdx,2
  00000001421B2831: mov         r8d,4
  00000001421B2837: call        __rust_dealloc
  00000001421B283C: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2844: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B284C: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2855: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B285E: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2867: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B286D: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2873: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2879: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B287F: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2885: add         rsp,0D8h
  00000001421B288C: pop         rbx
  00000001421B288D: pop         rdi
  00000001421B288E: pop         rsi
  00000001421B288F: pop         r12
  00000001421B2891: pop         r13
  00000001421B2893: pop         r14
  00000001421B2895: pop         r15
  00000001421B2897: pop         rbp
  00000001421B2898: ret
  00000001421B2899: nop         dword ptr [rax]
  00000001421B28A0: mov         qword ptr [rsp+10h],rdx
  00000001421B28A5: push        rbp
  00000001421B28A6: push        r15
  00000001421B28A8: push        r14
  00000001421B28AA: push        r13
  00000001421B28AC: push        r12
  00000001421B28AE: push        rsi
  00000001421B28AF: push        rdi
  00000001421B28B0: push        rbx
  00000001421B28B1: sub         rsp,0D8h
  00000001421B28B8: lea         rbp,[rdx+80h]
  00000001421B28BF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B28C5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B28CB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B28D1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B28D7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B28DD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B28E6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B28EF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B28F8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2900: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2908: mov         rdx,qword ptr [rbp+338h]
  00000001421B290F: test        rdx,rdx
  00000001421B2912: je          00000001421B292A
  00000001421B2914: mov         rcx,qword ptr [rbp+340h]
  00000001421B291B: shl         rdx,2
  00000001421B291F: mov         r8d,4
  00000001421B2925: call        __rust_dealloc
  00000001421B292A: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2932: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B293A: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2943: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B294C: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2955: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B295B: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2961: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2967: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B296D: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2973: add         rsp,0D8h
  00000001421B297A: pop         rbx
  00000001421B297B: pop         rdi
  00000001421B297C: pop         rsi
  00000001421B297D: pop         r12
  00000001421B297F: pop         r13
  00000001421B2981: pop         r14
  00000001421B2983: pop         r15
  00000001421B2985: pop         rbp
  00000001421B2986: ret
  00000001421B2987: nop         word ptr [rax+rax]
  00000001421B2990: mov         qword ptr [rsp+10h],rdx
  00000001421B2995: push        rbp
  00000001421B2996: push        r15
  00000001421B2998: push        r14
  00000001421B299A: push        r13
  00000001421B299C: push        r12
  00000001421B299E: push        rsi
  00000001421B299F: push        rdi
  00000001421B29A0: push        rbx
  00000001421B29A1: sub         rsp,0D8h
  00000001421B29A8: lea         rbp,[rdx+80h]
  00000001421B29AF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B29B5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B29BB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B29C1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B29C7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B29CD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B29D6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B29DF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B29E8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B29F0: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B29F8: mov         rdx,qword ptr [rbp+8]
  00000001421B29FC: test        rdx,rdx
  00000001421B29FF: je          00000001421B2A14
  00000001421B2A01: mov         rcx,qword ptr [rbp+10h]
  00000001421B2A05: shl         rdx,3
  00000001421B2A09: mov         r8d,4
  00000001421B2A0F: call        __rust_dealloc
  00000001421B2A14: mov         rdx,qword ptr [rbp+20h]
  00000001421B2A18: test        rdx,rdx
  00000001421B2A1B: je          00000001421B2A30
  00000001421B2A1D: mov         rcx,qword ptr [rbp+28h]
  00000001421B2A21: shl         rdx,2
  00000001421B2A25: mov         r8d,4
  00000001421B2A2B: call        __rust_dealloc
  00000001421B2A30: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2A38: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B2A40: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2A49: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B2A52: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2A5B: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B2A61: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2A67: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2A6D: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B2A73: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2A79: add         rsp,0D8h
  00000001421B2A80: pop         rbx
  00000001421B2A81: pop         rdi
  00000001421B2A82: pop         rsi
  00000001421B2A83: pop         r12
  00000001421B2A85: pop         r13
  00000001421B2A87: pop         r14
  00000001421B2A89: pop         r15
  00000001421B2A8B: pop         rbp
  00000001421B2A8C: ret
  00000001421B2A8D: nop         dword ptr [rax]
  00000001421B2A90: mov         qword ptr [rsp+10h],rdx
  00000001421B2A95: push        rbp
  00000001421B2A96: push        r15
  00000001421B2A98: push        r14
  00000001421B2A9A: push        r13
  00000001421B2A9C: push        r12
  00000001421B2A9E: push        rsi
  00000001421B2A9F: push        rdi
  00000001421B2AA0: push        rbx
  00000001421B2AA1: sub         rsp,0D8h
  00000001421B2AA8: lea         rbp,[rdx+80h]
  00000001421B2AAF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B2AB5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B2ABB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B2AC1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B2AC7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B2ACD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B2AD6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B2ADF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B2AE8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2AF0: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2AF8: mov         rdx,qword ptr [rbp+40h]
  00000001421B2AFC: test        rdx,rdx
  00000001421B2AFF: je          00000001421B2B14
  00000001421B2B01: mov         rcx,qword ptr [rbp+48h]
  00000001421B2B05: shl         rdx,3
  00000001421B2B09: mov         r8d,4
  00000001421B2B0F: call        __rust_dealloc
  00000001421B2B14: mov         rdx,qword ptr [rbp+58h]
  00000001421B2B18: test        rdx,rdx
  00000001421B2B1B: je          00000001421B2B30
  00000001421B2B1D: mov         rcx,qword ptr [rbp+60h]
  00000001421B2B21: shl         rdx,2
  00000001421B2B25: mov         r8d,4
  00000001421B2B2B: call        __rust_dealloc
  00000001421B2B30: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2B38: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B2B40: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2B49: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B2B52: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2B5B: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B2B61: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2B67: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2B6D: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B2B73: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2B79: add         rsp,0D8h
  00000001421B2B80: pop         rbx
  00000001421B2B81: pop         rdi
  00000001421B2B82: pop         rsi
  00000001421B2B83: pop         r12
  00000001421B2B85: pop         r13
  00000001421B2B87: pop         r14
  00000001421B2B89: pop         r15
  00000001421B2B8B: pop         rbp
  00000001421B2B8C: ret
  00000001421B2B8D: nop         dword ptr [rax]
  00000001421B2B90: mov         qword ptr [rsp+10h],rdx
  00000001421B2B95: push        rbp
  00000001421B2B96: push        r15
  00000001421B2B98: push        r14
  00000001421B2B9A: push        r13
  00000001421B2B9C: push        r12
  00000001421B2B9E: push        rsi
  00000001421B2B9F: push        rdi
  00000001421B2BA0: push        rbx
  00000001421B2BA1: sub         rsp,0D8h
  00000001421B2BA8: lea         rbp,[rdx+80h]
  00000001421B2BAF: movaps      xmmword ptr [rsp+30h],xmm15
  00000001421B2BB5: movaps      xmmword ptr [rsp+40h],xmm14
  00000001421B2BBB: movaps      xmmword ptr [rsp+50h],xmm13
  00000001421B2BC1: movaps      xmmword ptr [rsp+60h],xmm12
  00000001421B2BC7: movaps      xmmword ptr [rsp+70h],xmm11
  00000001421B2BCD: movaps      xmmword ptr [rsp+80h],xmm10
  00000001421B2BD6: movaps      xmmword ptr [rsp+90h],xmm9
  00000001421B2BDF: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001421B2BE8: movaps      xmmword ptr [rsp+0B0h],xmm7
  00000001421B2BF0: movaps      xmmword ptr [rsp+0C0h],xmm6
  00000001421B2BF8: mov         rdx,qword ptr [rbp+190h]
  00000001421B2BFF: test        rdx,rdx
  00000001421B2C02: je          00000001421B2C1A
  00000001421B2C04: mov         rcx,qword ptr [rbp+198h]
  00000001421B2C0B: shl         rdx,3
  00000001421B2C0F: mov         r8d,4
  00000001421B2C15: call        __rust_dealloc
  00000001421B2C1A: mov         rdx,qword ptr [rbp+1A8h]
  00000001421B2C21: test        rdx,rdx
  00000001421B2C24: je          00000001421B2C3C
  00000001421B2C26: mov         rcx,qword ptr [rbp+1B0h]
  00000001421B2C2D: shl         rdx,2
  00000001421B2C31: mov         r8d,4
  00000001421B2C37: call        __rust_dealloc
  00000001421B2C3C: movaps      xmm6,xmmword ptr [rsp+0C0h]
  00000001421B2C44: movaps      xmm7,xmmword ptr [rsp+0B0h]
  00000001421B2C4C: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001421B2C55: movaps      xmm9,xmmword ptr [rsp+90h]
  00000001421B2C5E: movaps      xmm10,xmmword ptr [rsp+80h]
  00000001421B2C67: movaps      xmm11,xmmword ptr [rsp+70h]
  00000001421B2C6D: movaps      xmm12,xmmword ptr [rsp+60h]
  00000001421B2C73: movaps      xmm13,xmmword ptr [rsp+50h]
  00000001421B2C79: movaps      xmm14,xmmword ptr [rsp+40h]
  00000001421B2C7F: movaps      xmm15,xmmword ptr [rsp+30h]
  00000001421B2C85: add         rsp,0D8h
  00000001421B2C8C: pop         rbx
  00000001421B2C8D: pop         rdi
  00000001421B2C8E: pop         rsi
  00000001421B2C8F: pop         r12
  00000001421B2C91: pop         r13
  00000001421B2C93: pop         r14
  00000001421B2C95: pop         r15
  00000001421B2C97: pop         rbp
  00000001421B2C98: ret
  00000001421B2C99: CC CC CC CC CC CC CC                             .......
; ==================================================================
; 排线闭包（assemble_roof_tiles → assemble_rectangular_roof::closure$2，经 Map::fold 内联）
; ---- _ZN102_$LT$core..iter..adapters..map..Map$LT$I$C$F$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4fold17had6773f64c7204beE
  00000001419F0790: push        rbp
  00000001419F0791: push        r15
  00000001419F0793: push        r14
  00000001419F0795: push        r13
  00000001419F0797: push        r12
  00000001419F0799: push        rsi
  00000001419F079A: push        rdi
  00000001419F079B: push        rbx
  00000001419F079C: sub         rsp,138h
  00000001419F07A3: lea         rbp,[rsp+80h]
  00000001419F07AB: movaps      xmmword ptr [rbp+0A0h],xmm14
  00000001419F07B3: movaps      xmmword ptr [rbp+90h],xmm13
  00000001419F07BB: movaps      xmmword ptr [rbp+80h],xmm12
  00000001419F07C3: movaps      xmmword ptr [rbp+70h],xmm11
  00000001419F07C8: movaps      xmmword ptr [rbp+60h],xmm10
  00000001419F07CD: movaps      xmmword ptr [rbp+50h],xmm9
  00000001419F07D2: movaps      xmmword ptr [rbp+40h],xmm8
  00000001419F07D7: movaps      xmmword ptr [rbp+30h],xmm7
  00000001419F07DB: movaps      xmmword ptr [rbp+20h],xmm6
  00000001419F07DF: mov         qword ptr [rbp+18h],0FFFFFFFFFFFFFFFEh
  00000001419F07E7: mov         ebx,dword ptr [rcx+38h]
  00000001419F07EA: mov         r14d,dword ptr [rcx+3Ch]
  00000001419F07EE: mov         rax,qword ptr [rdx]
  00000001419F07F1: mov         qword ptr [rbp],rax
  00000001419F07F5: mov         rax,qword ptr [rdx+8]
  00000001419F07F9: mov         qword ptr [rbp+10h],rax
  00000001419F07FD: cmp         ebx,r14d
  00000001419F0800: jge         00000001419F09A3
  00000001419F0806: mov         rax,qword ptr [rcx]
  00000001419F0809: mov         qword ptr [rbp-18h],rax
  00000001419F080D: mov         rsi,qword ptr [rcx+8]
  00000001419F0811: mov         rax,qword ptr [rcx+10h]
  00000001419F0815: mov         qword ptr [rbp-10h],rax
  00000001419F0819: mov         rax,qword ptr [rcx+18h]
  00000001419F081D: mov         qword ptr [rbp-8],rax
  00000001419F0821: mov         r15,qword ptr [rcx+20h]
  00000001419F0825: mov         r12,qword ptr [rcx+28h]
  00000001419F0829: mov         rdi,qword ptr [rcx+30h]
  00000001419F082D: mov         r13,qword ptr [rdx+10h]
  00000001419F0831: movss       xmm8,dword ptr [__real@bf800000]
  00000001419F083A: xorps       xmm9,xmm9
  00000001419F083E: movss       xmm10,dword ptr [__real@3f800000]
  00000001419F0847: nop         word ptr [rax+rax]
  00000001419F0850: xorps       xmm6,xmm6
  00000001419F0853: cvtsi2ss    xmm6,ebx
  00000001419F0857: mov         rax,qword ptr [rbp-18h]
  00000001419F085B: movss       xmm0,dword ptr [rax]
  00000001419F085F: addss       xmm0,xmm8
  00000001419F0864: divss       xmm6,xmm0
  00000001419F0868: movss       dword ptr [rbp+0Ch],xmm6
  00000001419F086D: ucomiss     xmm6,xmm9
  00000001419F0871: jb          00000001419F09F6
  00000001419F0877: ucomiss     xmm10,xmm6
  00000001419F087B: jb          00000001419F09F6
  00000001419F0881: ucomiss     xmm6,xmm6
  00000001419F0884: jp          00000001419F0A46
  00000001419F088A: mov         rcx,rsi
  00000001419F088D: movaps      xmm1,xmm6
  00000001419F0890: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001419F0895: mov         rdx,qword ptr [rsi+10h]
  00000001419F0899: mov         ecx,eax
  00000001419F089B: cmp         rdx,rcx
  00000001419F089E: jbe         00000001419F0A90
  00000001419F08A4: lea         rax,[rcx+1]
  00000001419F08A8: cmp         rax,rdx
  00000001419F08AB: jae         00000001419F0A7F
  00000001419F08B1: mov         rax,qword ptr [rsi+8]
  00000001419F08B5: movaps      xmm11,xmm10
  00000001419F08B9: subss       xmm11,xmm0
  00000001419F08BE: mulss       xmm11,dword ptr [rax+rcx*8]
  00000001419F08C4: movss       xmm14,dword ptr [rax+rcx*8+8]
  00000001419F08CB: mov         rax,qword ptr [rbp-10h]
  00000001419F08CF: movsd       xmm12,mmword ptr [rax]
  00000001419F08D4: movhps      xmm12,qword ptr [r15]
  00000001419F08D8: mulss       xmm14,xmm0
  00000001419F08DD: mov         rax,qword ptr [rbp-8]
  00000001419F08E1: movsd       xmm13,mmword ptr [rax]
  00000001419F08E6: movhps      xmm13,qword ptr [r12]
  00000001419F08EB: mov         rcx,rdi
  00000001419F08EE: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  00000001419F08F3: movaps      xmm7,xmm0
  00000001419F08F6: mov         rcx,rsi
  00000001419F08F9: movaps      xmm1,xmm6
  00000001419F08FC: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h5fd0a52d9eb45b8bE.llvm.9475246483403468623
  00000001419F0901: mov         rdx,qword ptr [rsi+10h]
  00000001419F0905: mov         ecx,eax
  00000001419F0907: cmp         rdx,rcx
  00000001419F090A: jbe         00000001419F0A71
  00000001419F0910: lea         rax,[rcx+1]
  00000001419F0914: cmp         rax,rdx
  00000001419F0917: jae         00000001419F0A60
  00000001419F091D: inc         ebx
  00000001419F091F: addss       xmm11,xmm14
  00000001419F0924: movaps      xmm1,xmm10
  00000001419F0928: subss       xmm1,xmm11
  00000001419F092D: shufps      xmm1,xmm1,0
  00000001419F0931: mulps       xmm12,xmm1
  00000001419F0935: shufps      xmm11,xmm11,0
  00000001419F093A: mulps       xmm11,xmm13
  00000001419F093E: addps       xmm11,xmm12
  00000001419F0942: mov         rax,qword ptr [rsi+8]
  00000001419F0946: movaps      xmm1,xmm10
  00000001419F094A: subss       xmm1,xmm0
  00000001419F094E: mulss       xmm1,dword ptr [rax+rcx*8+4]
  00000001419F0954: movss       xmm2,dword ptr [rax+rcx*8+0Ch]
  00000001419F095A: mulss       xmm2,xmm0
  00000001419F095E: addss       xmm2,xmm1
  00000001419F0962: movaps      xmm0,xmm10
  00000001419F0966: subss       xmm0,xmm2
  00000001419F096A: mulss       xmm0,xmm9
  00000001419F096F: mulss       xmm7,xmm2
  00000001419F0973: addss       xmm7,xmm0
  00000001419F0977: mov         rcx,qword ptr [rbp+10h]
  00000001419F097B: lea         rax,[rcx+rcx*2]
  00000001419F097F: movups      xmmword ptr [r13+rax*8],xmm11
  00000001419F0985: movss       dword ptr [r13+rax*8+10h],xmm6
  00000001419F098C: movss       dword ptr [r13+rax*8+14h],xmm7
  00000001419F0993: inc         rcx
  00000001419F0996: mov         qword ptr [rbp+10h],rcx
  00000001419F099A: cmp         ebx,r14d
  00000001419F099D: jne         00000001419F0850
  00000001419F09A3: mov         rax,qword ptr [rbp]
  00000001419F09A7: mov         rcx,qword ptr [rbp+10h]
  00000001419F09AB: mov         qword ptr [rax],rcx
  00000001419F09AE: movaps      xmm6,xmmword ptr [rbp+20h]
  00000001419F09B2: movaps      xmm7,xmmword ptr [rbp+30h]
  00000001419F09B6: movaps      xmm8,xmmword ptr [rbp+40h]
  00000001419F09BB: movaps      xmm9,xmmword ptr [rbp+50h]
  00000001419F09C0: movaps      xmm10,xmmword ptr [rbp+60h]
  00000001419F09C5: movaps      xmm11,xmmword ptr [rbp+70h]
  00000001419F09CA: movaps      xmm12,xmmword ptr [rbp+80h]
  00000001419F09D2: movaps      xmm13,xmmword ptr [rbp+90h]
  00000001419F09DA: movaps      xmm14,xmmword ptr [rbp+0A0h]
  00000001419F09E2: add         rsp,138h
  00000001419F09E9: pop         rbx
  00000001419F09EA: pop         rdi
  00000001419F09EB: pop         rsi
  00000001419F09EC: pop         r12
  00000001419F09EE: pop         r13
  00000001419F09F0: pop         r14
  00000001419F09F2: pop         r15
  00000001419F09F4: pop         rbp
  00000001419F09F5: ret
  00000001419F09F6: lea         rax,[rbp+0Ch]
  00000001419F09FA: mov         qword ptr [rbp-28h],rax
  00000001419F09FE: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  00000001419F0A05: mov         qword ptr [rbp-20h],rax
  00000001419F0A09: lea         rax,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.1.llvm.17463645933293978377]
  00000001419F0A10: mov         qword ptr [rbp-58h],rax
  00000001419F0A14: mov         qword ptr [rbp-50h],1
  00000001419F0A1C: mov         qword ptr [rbp-38h],0
  00000001419F0A24: lea         rax,[rbp-28h]
  00000001419F0A28: mov         qword ptr [rbp-48h],rax
  00000001419F0A2C: mov         qword ptr [rbp-40h],1
  00000001419F0A34: lea         rdx,[anon.ef2a7ba9c09612c12f357258af8d980b.3.llvm.477464086015353327]
  00000001419F0A3B: lea         rcx,[rbp-58h]
  00000001419F0A3F: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  00000001419F0A44: jmp         00000001419F0A9C
  00000001419F0A46: lea         rcx,[anon.6eb2c319c8ba7444a890b1dfc3e489f9.2.llvm.17463645933293978377]
  00000001419F0A4D: lea         r8,[anon.ef2a7ba9c09612c12f357258af8d980b.3.llvm.477464086015353327]
  00000001419F0A54: mov         edx,1Dh
  00000001419F0A59: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001419F0A5E: jmp         00000001419F0A9C
  00000001419F0A60: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.15.llvm.9475246483403468623]
  00000001419F0A67: mov         rcx,rax
  00000001419F0A6A: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001419F0A6F: jmp         00000001419F0A9C
  00000001419F0A71: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.14.llvm.9475246483403468623]
  00000001419F0A78: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001419F0A7D: jmp         00000001419F0A9C
  00000001419F0A7F: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.15.llvm.9475246483403468623]
  00000001419F0A86: mov         rcx,rax
  00000001419F0A89: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001419F0A8E: jmp         00000001419F0A9C
  00000001419F0A90: lea         r8,[anon.ecd1c29136b20f1a2a2a1422b1db68a6.14.llvm.9475246483403468623]
  00000001419F0A97: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  00000001419F0A9C: ud2
  00000001419F0A9E: nop
  00000001419F0AA0: mov         qword ptr [rsp+10h],rdx
  00000001419F0AA5: push        rbp
  00000001419F0AA6: push        r15
  00000001419F0AA8: push        r14
  00000001419F0AAA: push        r13
  00000001419F0AAC: push        r12
  00000001419F0AAE: push        rsi
  00000001419F0AAF: push        rdi
  00000001419F0AB0: push        rbx
  00000001419F0AB1: sub         rsp,0B8h
  00000001419F0AB8: lea         rbp,[rdx+80h]
  00000001419F0ABF: movaps      xmmword ptr [rsp+20h],xmm14
  00000001419F0AC5: movaps      xmmword ptr [rsp+30h],xmm13
  00000001419F0ACB: movaps      xmmword ptr [rsp+40h],xmm12
  00000001419F0AD1: movaps      xmmword ptr [rsp+50h],xmm11
  00000001419F0AD7: movaps      xmmword ptr [rsp+60h],xmm10
  00000001419F0ADD: movaps      xmmword ptr [rsp+70h],xmm9
  00000001419F0AE3: movaps      xmmword ptr [rsp+80h],xmm8
  00000001419F0AEC: movaps      xmmword ptr [rsp+90h],xmm7
  00000001419F0AF4: movaps      xmmword ptr [rsp+0A0h],xmm6
  00000001419F0AFC: mov         rax,qword ptr [rbp]
  00000001419F0B00: mov         rcx,qword ptr [rbp+10h]
  00000001419F0B04: mov         qword ptr [rax],rcx
  00000001419F0B07: movaps      xmm6,xmmword ptr [rsp+0A0h]
  00000001419F0B0F: movaps      xmm7,xmmword ptr [rsp+90h]
  00000001419F0B17: movaps      xmm8,xmmword ptr [rsp+80h]
  00000001419F0B20: movaps      xmm9,xmmword ptr [rsp+70h]
  00000001419F0B26: movaps      xmm10,xmmword ptr [rsp+60h]
  00000001419F0B2C: movaps      xmm11,xmmword ptr [rsp+50h]
  00000001419F0B32: movaps      xmm12,xmmword ptr [rsp+40h]
  00000001419F0B38: movaps      xmm13,xmmword ptr [rsp+30h]
  00000001419F0B3E: movaps      xmm14,xmmword ptr [rsp+20h]
  00000001419F0B44: add         rsp,0B8h
  00000001419F0B4B: pop         rbx
  00000001419F0B4C: pop         rdi
  00000001419F0B4D: pop         rsi
  00000001419F0B4E: pop         r12
  00000001419F0B50: pop         r13
  00000001419F0B52: pop         r14
  00000001419F0B54: pop         r15
  00000001419F0B56: pop         rbp
  00000001419F0B57: ret
  00000001419F0B58: CC CC CC CC CC CC CC CC                          ........

; ==================================================================
; utils::random_splits / random_splits_into
; ---- _ZN5utils13random_splits17h445a73628407a507E
  0000000140C90350: push        rbp
  0000000140C90351: push        r15
  0000000140C90353: push        r14
  0000000140C90355: push        r12
  0000000140C90357: push        rsi
  0000000140C90358: push        rdi
  0000000140C90359: push        rbx
  0000000140C9035A: sub         rsp,60h
  0000000140C9035E: lea         rbp,[rsp+60h]
  0000000140C90363: movaps      xmmword ptr [rbp-10h],xmm6
  0000000140C90367: mov         qword ptr [rbp-18h],0FFFFFFFFFFFFFFFEh
  0000000140C9036F: mov         rdi,qword ptr [rbp+60h]
  0000000140C90373: cmp         rdx,1
  0000000140C90377: jbe         0000000140C9041F
  0000000140C9037D: mov         r14,rdx
  0000000140C90380: lea         r15,[rdx*4]
  0000000140C90388: xor         r12d,r12d
  0000000140C9038B: mov         rax,rdx
  0000000140C9038E: shr         rax,3Eh
  0000000140C90392: jne         0000000140C90433
  0000000140C90398: mov         rax,7FFFFFFFFFFFFFFCh
  0000000140C903A2: cmp         r15,rax
  0000000140C903A5: ja          0000000140C90433
  0000000140C903AB: mov         rbx,r9
  0000000140C903AE: movaps      xmm6,xmm2
  0000000140C903B1: mov         rsi,rcx
  0000000140C903B4: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  0000000140C903BB: mov         r12d,4
  0000000140C903C1: mov         edx,4
  0000000140C903C6: mov         rcx,r15
  0000000140C903C9: call        __rust_alloc
  0000000140C903CE: test        rax,rax
  0000000140C903D1: je          0000000140C90433
  0000000140C903D3: mov         qword ptr [rbp-30h],r14
  0000000140C903D7: mov         qword ptr [rbp-28h],rax
  0000000140C903DB: mov         qword ptr [rbp-20h],0
  0000000140C903E3: mov         qword ptr [rsp+20h],rdi
  0000000140C903E8: lea         r9,[rbp-30h]
  0000000140C903EC: mov         rcx,r14
  0000000140C903EF: movaps      xmm1,xmm6
  0000000140C903F2: mov         r8,rbx
  0000000140C903F5: call        _ZN5utils18random_splits_into17ha77e070c5a563e57E
  0000000140C903FA: mov         rax,qword ptr [rbp-20h]
  0000000140C903FE: mov         qword ptr [rsi+10h],rax
  0000000140C90402: movups      xmm0,xmmword ptr [rbp-30h]
  0000000140C90406: movups      xmmword ptr [rsi],xmm0
  0000000140C90409: mov         rax,rsi
  0000000140C9040C: movaps      xmm6,xmmword ptr [rbp-10h]
  0000000140C90410: add         rsp,60h
  0000000140C90414: pop         rbx
  0000000140C90415: pop         rdi
  0000000140C90416: pop         rsi
  0000000140C90417: pop         r12
  0000000140C90419: pop         r14
  0000000140C9041B: pop         r15
  0000000140C9041D: pop         rbp
  0000000140C9041E: ret
  0000000140C9041F: lea         rcx,[142B2D880h]
  0000000140C90426: mov         edx,1Dh
  0000000140C9042B: mov         r8,rdi
  0000000140C9042E: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  0000000140C90433: mov         rcx,r12
  0000000140C90436: mov         rdx,r15
  0000000140C90439: mov         r8,rdi
  0000000140C9043C: call        _ZN5alloc7raw_vec12handle_error17hb99beac7784c4464E
  0000000140C90441: int         3
  0000000140C90442: nop         word ptr cs:[rax+rax]
  0000000140C90450: mov         qword ptr [rsp+10h],rdx
  0000000140C90455: push        rbp
  0000000140C90456: push        r15
  0000000140C90458: push        r14
  0000000140C9045A: push        r12
  0000000140C9045C: push        rsi
  0000000140C9045D: push        rdi
  0000000140C9045E: push        rbx
  0000000140C9045F: sub         rsp,40h
  0000000140C90463: lea         rbp,[rdx+60h]
  0000000140C90467: movaps      xmmword ptr [rsp+20h],xmm6
  0000000140C9046C: mov         rdx,qword ptr [rbp-30h]
  0000000140C90470: test        rdx,rdx
  0000000140C90473: je          0000000140C90488
  0000000140C90475: mov         rcx,qword ptr [rbp-28h]
  0000000140C90479: shl         rdx,2
  0000000140C9047D: mov         r8d,4
  0000000140C90483: call        __rust_dealloc
  0000000140C90488: movaps      xmm6,xmmword ptr [rsp+20h]
  0000000140C9048D: add         rsp,40h
  0000000140C90491: pop         rbx
  0000000140C90492: pop         rdi
  0000000140C90493: pop         rsi
  0000000140C90494: pop         r12
  0000000140C90496: pop         r14
  0000000140C90498: pop         r15
  0000000140C9049A: pop         rbp
  0000000140C9049B: ret
  0000000140C9049C: CC CC CC CC                                      ....
; ---- _ZN5utils18random_splits_into17ha77e070c5a563e57E
  0000000140C90050: push        rbp
  0000000140C90051: push        r15
  0000000140C90053: push        r14
  0000000140C90055: push        r13
  0000000140C90057: push        r12
  0000000140C90059: push        rsi
  0000000140C9005A: push        rdi
  0000000140C9005B: push        rbx
  0000000140C9005C: sub         rsp,98h
  0000000140C90063: lea         rbp,[rsp+80h]
  0000000140C9006B: movaps      xmmword ptr [rbp],xmm9
  0000000140C90070: movaps      xmmword ptr [rbp-10h],xmm8
  0000000140C90075: movaps      xmmword ptr [rbp-20h],xmm7
  0000000140C90079: movaps      xmmword ptr [rbp-30h],xmm6
  0000000140C9007D: mov         qword ptr [rbp-38h],0FFFFFFFFFFFFFFFEh
  0000000140C90085: mov         rsi,r9
  0000000140C90088: cmp         rcx,3
  0000000140C9008C: jae         0000000140C900BD
  0000000140C9008E: mov         rax,qword ptr [rsi]
  0000000140C90091: mov         rdx,qword ptr [rsi+10h]
  0000000140C90095: sub         rax,rdx
  0000000140C90098: cmp         rax,1
  0000000140C9009C: jbe         0000000140C9028A
  0000000140C900A2: mov         rax,qword ptr [rsi+8]
  0000000140C900A6: add         rsi,10h
  0000000140C900AA: mov         rcx,3F80000000000000h
  0000000140C900B4: mov         qword ptr [rax+rdx*4],rcx
  0000000140C900B8: jmp         0000000140C9025D
  0000000140C900BD: mov         rdi,r8
  0000000140C900C0: movaps      xmm6,xmm1
  0000000140C900C3: mov         rbx,rcx
  0000000140C900C6: mov         rax,rcx
  0000000140C900C9: dec         rax
  0000000140C900CC: js          0000000140C900D5
  0000000140C900CE: cvtsi2ss    xmm0,rax
  0000000140C900D3: jmp         0000000140C900EA
  0000000140C900D5: mov         rcx,rax
  0000000140C900D8: shr         rcx,1
  0000000140C900DB: and         eax,1
  0000000140C900DE: or          rax,rcx
  0000000140C900E1: cvtsi2ss    xmm0,rax
  0000000140C900E6: addss       xmm0,xmm0
  0000000140C900EA: mov         r14,qword ptr [rbp+80h]
  0000000140C900F1: movss       xmm7,dword ptr [__real@3f800000]
  0000000140C900F9: movaps      xmm8,xmm7
  0000000140C900FD: divss       xmm8,xmm0
  0000000140C90102: mulss       xmm8,dword ptr [__real@3efd70a4]
  0000000140C9010B: movaps      xmm9,xmm8
  0000000140C9010F: minss       xmm9,xmm6
  0000000140C90114: cmpunordss  xmm6,xmm6
  0000000140C90119: mov         r15,qword ptr [rsi+10h]
  0000000140C9011D: cmp         r15,qword ptr [rsi]
  0000000140C90120: jne         0000000140C9012D
  0000000140C90122: mov         rcx,rsi
  0000000140C90125: mov         rdx,r14
  0000000140C90128: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h081e4b9c7b1fd67bE
  0000000140C9012D: andps       xmm8,xmm6
  0000000140C90131: andnps      xmm6,xmm9
  0000000140C90135: mov         rax,qword ptr [rsi+8]
  0000000140C90139: mov         dword ptr [rax+r15*4],0
  0000000140C90141: inc         r15
  0000000140C90144: mov         qword ptr [rsi+10h],r15
  0000000140C90148: test        rbx,rbx
  0000000140C9014B: js          0000000140C90158
  0000000140C9014D: xorps       xmm9,xmm9
  0000000140C90151: cvtsi2ss    xmm9,rbx
  0000000140C90156: jmp         0000000140C90174
  0000000140C90158: mov         rax,rbx
  0000000140C9015B: shr         rax,1
  0000000140C9015E: mov         ecx,ebx
  0000000140C90160: and         ecx,1
  0000000140C90163: or          rcx,rax
  0000000140C90166: xorps       xmm9,xmm9
  0000000140C9016A: cvtsi2ss    xmm9,rcx
  0000000140C9016F: addss       xmm9,xmm9
  0000000140C90174: orps        xmm6,xmm8
  0000000140C90178: addss       xmm9,dword ptr [__real@bf800000]
  0000000140C90181: add         rbx,0FFFFFFFFFFFFFFFEh
  0000000140C90185: mov         rax,qword ptr [rsi]
  0000000140C90188: sub         rax,r15
  0000000140C9018B: cmp         rax,rbx
  0000000140C9018E: jb          0000000140C902B0
  0000000140C90194: lea         rax,[rsi+10h]
  0000000140C90198: mov         qword ptr [rbp-48h],rax
  0000000140C9019C: divss       xmm7,xmm9
  0000000140C901A1: mov         qword ptr [rbp-50h],r15
  0000000140C901A5: lea         r15,[r15*4]
  0000000140C901AD: mov         r12,qword ptr [rsi+8]
  0000000140C901B1: add         r12,r15
  0000000140C901B4: mov         qword ptr [rbp-40h],0
  0000000140C901BC: movss       xmm8,dword ptr [__real@bf000000]
  0000000140C901C5: jmp         0000000140C901FC
  0000000140C901C7: nop         word ptr [rax+rax]
  0000000140C901D0: xorps       xmm1,xmm1
  0000000140C901D3: cvtsi2ss    xmm1,r13
  0000000140C901D8: mulss       xmm1,xmm7
  0000000140C901DC: addss       xmm0,xmm8
  0000000140C901E1: mulss       xmm0,xmm6
  0000000140C901E5: addss       xmm0,xmm1
  0000000140C901E9: mov         rax,qword ptr [rbp-40h]
  0000000140C901ED: movss       dword ptr [r12+rax*4],xmm0
  0000000140C901F3: mov         qword ptr [rbp-40h],r13
  0000000140C901F7: cmp         rbx,r13
  0000000140C901FA: je          0000000140C9022A
  0000000140C901FC: mov         rcx,rdi
  0000000140C901FF: call        _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E
  0000000140C90204: mov         r13,qword ptr [rbp-40h]
  0000000140C90208: inc         r13
  0000000140C9020B: jns         0000000140C901D0
  0000000140C9020D: mov         rax,r13
  0000000140C90210: shr         rax,1
  0000000140C90213: mov         ecx,r13d
  0000000140C90216: and         ecx,1
  0000000140C90219: or          rcx,rax
  0000000140C9021C: xorps       xmm1,xmm1
  0000000140C9021F: cvtsi2ss    xmm1,rcx
  0000000140C90224: addss       xmm1,xmm1
  0000000140C90228: jmp         0000000140C901D8
  0000000140C9022A: mov         rdi,qword ptr [rbp-50h]
  0000000140C9022E: lea         rax,[rdi+r13]
  0000000140C90232: mov         qword ptr [rsi+10h],rax
  0000000140C90236: cmp         rax,qword ptr [rsi]
  0000000140C90239: jne         0000000140C90246
  0000000140C9023B: mov         rcx,rsi
  0000000140C9023E: mov         rdx,r14
  0000000140C90241: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h081e4b9c7b1fd67bE
  0000000140C90246: lea         rdx,[rdi+r13]
  0000000140C9024A: dec         rdx
  0000000140C9024D: add         r15,qword ptr [rsi+8]
  0000000140C90251: mov         dword ptr [r15+r13*4],3F800000h
  0000000140C90259: mov         rsi,qword ptr [rbp-48h]
  0000000140C9025D: add         rdx,2
  0000000140C90261: mov         qword ptr [rsi],rdx
  0000000140C90264: movaps      xmm6,xmmword ptr [rbp-30h]
  0000000140C90268: movaps      xmm7,xmmword ptr [rbp-20h]
  0000000140C9026C: movaps      xmm8,xmmword ptr [rbp-10h]
  0000000140C90271: movaps      xmm9,xmmword ptr [rbp]
  0000000140C90276: add         rsp,98h
  0000000140C9027D: pop         rbx
  0000000140C9027E: pop         rdi
  0000000140C9027F: pop         rsi
  0000000140C90280: pop         r12
  0000000140C90282: pop         r13
  0000000140C90284: pop         r14
  0000000140C90286: pop         r15
  0000000140C90288: pop         rbp
  0000000140C90289: ret
  0000000140C9028A: mov         qword ptr [rsp+20h],4
  0000000140C90293: mov         r8d,2
  0000000140C90299: mov         r9d,4
  0000000140C9029F: mov         rcx,rsi
  0000000140C902A2: call        _ZN5alloc7raw_vec20RawVecInner$LT$A$GT$7reserve21do_reserve_and_handle17h9b7ecf590540f26bE
  0000000140C902A7: mov         rdx,qword ptr [rsi+10h]
  0000000140C902AB: jmp         0000000140C900A2
  0000000140C902B0: mov         qword ptr [rsp+20h],4
  0000000140C902B9: mov         r9d,4
  0000000140C902BF: mov         rcx,rsi
  0000000140C902C2: mov         rdx,r15
  0000000140C902C5: mov         r8,rbx
  0000000140C902C8: call        _ZN5alloc7raw_vec20RawVecInner$LT$A$GT$7reserve21do_reserve_and_handle17h9b7ecf590540f26bE
  0000000140C902CD: mov         r15,qword ptr [rsi+10h]
  0000000140C902D1: jmp         0000000140C90194
  0000000140C902D6: nop         word ptr cs:[rax+rax]
  0000000140C902E0: mov         qword ptr [rsp+10h],rdx
  0000000140C902E5: push        rbp
  0000000140C902E6: push        r15
  0000000140C902E8: push        r14
  0000000140C902EA: push        r13
  0000000140C902EC: push        r12
  0000000140C902EE: push        rsi
  0000000140C902EF: push        rdi
  0000000140C902F0: push        rbx
  0000000140C902F1: sub         rsp,68h
  0000000140C902F5: lea         rbp,[rdx+80h]
  0000000140C902FC: movaps      xmmword ptr [rsp+20h],xmm9
  0000000140C90302: movaps      xmmword ptr [rsp+30h],xmm8
  0000000140C90308: movaps      xmmword ptr [rsp+40h],xmm7
  0000000140C9030D: movaps      xmmword ptr [rsp+50h],xmm6
  0000000140C90312: mov         rax,qword ptr [rbp-50h]
  0000000140C90316: add         rax,qword ptr [rbp-40h]
  0000000140C9031A: mov         rcx,qword ptr [rbp-48h]
  0000000140C9031E: mov         qword ptr [rcx],rax
  0000000140C90321: movaps      xmm6,xmmword ptr [rsp+50h]
  0000000140C90326: movaps      xmm7,xmmword ptr [rsp+40h]
  0000000140C9032B: movaps      xmm8,xmmword ptr [rsp+30h]
  0000000140C90331: movaps      xmm9,xmmword ptr [rsp+20h]
  0000000140C90337: add         rsp,68h
  0000000140C9033B: pop         rbx
  0000000140C9033C: pop         rdi
  0000000140C9033D: pop         rsi
  0000000140C9033E: pop         r12
  0000000140C90340: pop         r13
  0000000140C90342: pop         r14
  0000000140C90344: pop         r15
  0000000140C90346: pop         rbp
  0000000140C90347: ret
  0000000140C90348: CC CC CC CC CC CC CC CC                          ........
