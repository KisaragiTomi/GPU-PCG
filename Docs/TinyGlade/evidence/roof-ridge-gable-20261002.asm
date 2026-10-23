; RE_RoofRidgeGable.md（附录 F：脊长可调 / 双坡 + 山墙，2026-10-02）的反汇编证据
; 来源：D:/MyProject/Tiny Glade/tiny-glade.exe + tiny_glade.pdb，dumpbin /DISASM:NOBYTES 全量流式过滤（方法同卷零「证据来源」）。
; __real@xxxxxxxx 是单精度字面量（TG 米制），__xmm@ 按高位在前写。地址是 exe 内 VA。
; assemble_rectangular_roof 的全文在同目录 roof-tiles-assemble-20260929.asm，这里只剪与脊长 / 山墙有关的几段。
;
; 读法速查
; ── 参数与判据
;   1408E2690 / 1408E2700  RoofRidgeDims::from / calc：脊长 = (1 − r)·0.1 + r·顺脊边长；另一维恒 0.1；非矩形恒 (0.1, 0.1)
;   1408E30F0  inverse_ridge_01_param：r = clamp((len − 0.1) / (side − 0.1), 0, 1) —— 钳到 1 的那一下就是山墙的来源
;   1408E33B0  Roof::is_gable：ridge_length_01 == 1.0（cmpeqps）且 height_01 > 0.1
;   1408E33E0  Roof::gable_sides：Some((dir, dir | 2)) —— 两条山墙边的边号
;   1408E3B60  ridge_length_01_from_rectangle_ratio：smoothstep(clamp((max(a/b, b/a) + 1.5·smoothstep((max(a, b) − 2.25) / 5.25) − 1.5) / 1.5))
;   1408E27E2  to_world_space：0.1 >= max(dims) ⇒ 单点（尖）；否则两个端点 = 矩形中心 ∓ dims / 2（tip offset 在 1408E2822 被乘 0）
; ── 屋面
;   140A88107  檐口矩形 = 墙矩形 + 0.28 + eave_length_ws（两轴整边加量）；140A8812E 起脊矩形 = RoofRidgeDims
;   1421AF066  is_gable 时：c = style == 0 ? 0.81 : 0.25（1421AF07E / 1421AF0BE）；d = c + eave（1421AF0AA）；style == 0 取负（1421AF0F2）；
;              檐口矩形顺脊长 += d，并把同一个值写进脊矩形（1421AF0FD / 1421AF101）⇒ 两端的面竖直
;   1421B0B9D / 1421B0BAA  坡面循环里面号等于 gable_sides 任一个就 continue；四条角线盖板的循环（1421AF1AC 起）没有这道判断
;   1421AFEF4  山墙的水平脊盖板：style == 1 两端各 +0.56，否则各 −0.28（0.56 × −0.5）
;   1421B0E39  逐瓦裁剪面常数项里的 −0.075
; ── 墙
;   14078C19A..14078C1A3  c = is_halftimbered ? 0 : 0.46；14078C1D3 powf(1 − 2|t| / (W / half), k)；14078C1D8..14078C1FF 墙顶 += H·p + c
;   14078BFB2..14078BFF5  W = half + 0.28 + (半木构 ? 0 : inverse_lerp(0.5, 1, profile)·eave)
;   14078C404..14078C424  max_y_gable = max_y + Roof::height() + 0.46
;   14123F249..14123F278  trim_rows 的行端吸附步长：山墙 0.2 块砖、否则 0.5 块（一块砖 0.46 m）；14123F323 起不足一块砖的行段加宽
; ── 手柄
;   1421AA44D  触发盒中心 = 端点 + dir·0.75；1421AA58F box_trigger_volume(1.0, 1.0, 1.5)
;   1421A963C..1421A9689  起拖：offset = 2·|P0 − C| − |p0 − p1|；1421A9794..1421A97C1 拖拽中：len = 2·|P − C| − offset
;   1421A9FA5  箭头 scale × 2；1421A9FD1 位置 = 端点 + dir·0.2
; ── 默认值与同步
;   140A9413A..140A94166  create_rectangle 写进 CreateRoofCmd：tip (0,0)、profile 0.5、eave_01 0.2、ridge_dir = (size.y > size.x)、ridge_length_01 = 上面那个公式
;   1421AB16B  create_roof：gable_style = WallStyle::is_halftimbered(wall.style)；1421A79AF 起 update_roof_spatial 每次墙变再对齐
;   1419EEA30..1419EEA72  place_roof_spires：max(d.x / d.y, d.y / d.x) < 1.01 才放 ⇒ 有脊的屋顶没有塔尖

; ==================================================================
; roof_ridge::RoofRidgeDims::from / ::calc（全文）
; _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims4from17hcb00f4a21c308a29E:
  00000001408E2690: cmp         dword ptr [rcx+8],1
  00000001408E2694: jne         00000001408E26CF
  00000001408E2696: movss       xmm1,dword ptr [rcx+34h]
  00000001408E269B: movss       xmm0,dword ptr [__real@3f800000]
  00000001408E26A3: subss       xmm0,xmm1
  00000001408E26A7: mulss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E26AF: cmp         byte ptr [rcx+3Ch],0
  00000001408E26B3: je          00000001408E26DA
  00000001408E26B5: mulss       xmm1,dword ptr [rcx+20h]
  00000001408E26BA: addss       xmm0,xmm1
  00000001408E26BE: movd        eax,xmm0
  00000001408E26C2: shl         rax,20h
  00000001408E26C6: mov         ecx,3DCCCCCDh
  00000001408E26CB: or          rax,rcx
  00000001408E26CE: ret
  00000001408E26CF: mov         rax,3DCCCCCD3DCCCCCDh
  00000001408E26D9: ret
  00000001408E26DA: mulss       xmm1,dword ptr [rcx+1Ch]
  00000001408E26DF: addss       xmm0,xmm1
  00000001408E26E3: movd        ecx,xmm0
  00000001408E26E7: mov         rax,3DCCCCCD00000000h
  00000001408E26F1: or          rax,rcx
  00000001408E26F4: ret
  00000001408E26F5: CC CC CC CC CC CC CC CC CC CC CC                 ...........
; _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims4calc17hd1c4a42158756616E:
  00000001408E2700: movss       xmm1,dword ptr [__real@3f800000]
  00000001408E2708: subss       xmm1,xmm0
  00000001408E270C: mulss       xmm1,dword ptr [__real@3dcccccd]
  00000001408E2714: test        dl,dl
  00000001408E2716: je          00000001408E2731
  00000001408E2718: mulss       xmm0,xmm3
  00000001408E271C: addss       xmm1,xmm0
  00000001408E2720: movd        eax,xmm1
  00000001408E2724: shl         rax,20h
  00000001408E2728: mov         ecx,3DCCCCCDh
  00000001408E272D: or          rax,rcx
  00000001408E2730: ret
  00000001408E2731: mulss       xmm0,xmm2
  00000001408E2735: addss       xmm1,xmm0
  00000001408E2739: movd        ecx,xmm1
  00000001408E273D: mov         rax,3DCCCCCD00000000h
  00000001408E2747: or          rax,rcx
  00000001408E274A: ret

; ==================================================================
; roof_ridge::RoofRidgeDims::to_world_space（全文）
; _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims14to_world_space17h36c23ee40b8867c8E:
  00000001408E2750: push        rsi
  00000001408E2751: push        rdi
  00000001408E2752: sub         rsp,138h
  00000001408E2759: movaps      xmmword ptr [rsp+120h],xmm15
  00000001408E2762: movaps      xmmword ptr [rsp+110h],xmm14
  00000001408E276B: movaps      xmmword ptr [rsp+100h],xmm13
  00000001408E2774: movaps      xmmword ptr [rsp+0F0h],xmm12
  00000001408E277D: movaps      xmmword ptr [rsp+0E0h],xmm11
  00000001408E2786: movaps      xmmword ptr [rsp+0D0h],xmm10
  00000001408E278F: movaps      xmmword ptr [rsp+0C0h],xmm9
  00000001408E2798: movaps      xmmword ptr [rsp+0B0h],xmm8
  00000001408E27A1: movaps      xmmword ptr [rsp+0A0h],xmm7
  00000001408E27A9: movaps      xmmword ptr [rsp+90h],xmm6
  00000001408E27B1: mov         rdi,r8
  00000001408E27B4: mov         rsi,rcx
  00000001408E27B7: movss       xmm13,dword ptr [rdx]
  00000001408E27BC: movss       xmm14,dword ptr [rdx+4]
  00000001408E27C2: movaps      xmm0,xmm13
  00000001408E27C6: cmpunordss  xmm0,xmm13
  00000001408E27CC: movaps      xmm1,xmm0
  00000001408E27CF: andps       xmm1,xmm14
  00000001408E27D3: movaps      xmm2,xmm14
  00000001408E27D7: maxss       xmm2,xmm13
  00000001408E27DC: andnps      xmm0,xmm2
  00000001408E27DF: orps        xmm0,xmm1
  00000001408E27E2: movss       xmm1,dword ptr [__real@3dcccccd]
  00000001408E27EA: ucomiss     xmm1,xmm0
  00000001408E27ED: jae         00000001408E2AA0
  00000001408E27F3: test        byte ptr [rdi+8],1
  00000001408E27F7: je          00000001408E2E73
  00000001408E27FD: mov         rax,qword ptr [rdi+1Ch]
  00000001408E2801: mov         qword ptr [rsp+60h],rax
  00000001408E2806: movups      xmm0,xmmword ptr [rdi+0Ch]
  00000001408E280A: movaps      xmmword ptr [rsp+50h],xmm0
  00000001408E280F: movsd       xmm9,mmword ptr [rdi+1Ch]
  00000001408E2815: movsd       xmm11,mmword ptr [rdi+24h]
  00000001408E281B: mulps       xmm11,xmm9
  00000001408E281F: xorps       xmm6,xmm6
  00000001408E2822: mulps       xmm11,xmm6
  00000001408E2826: cmp         byte ptr [rdi+3Ch],1
  00000001408E282A: jne         00000001408E2B26
  00000001408E2830: lea         rcx,[rsp+40h]
  00000001408E2835: lea         rdx,[rsp+50h]
  00000001408E283A: call        _ZN5utils8geometry9rectangle11Rectangle2d16orientation_mat217h578587259d3307efE
  00000001408E283F: mulss       xmm14,dword ptr [__real@3f000000]
  00000001408E2848: xorps       xmm7,xmm7
  00000001408E284B: movshdup    xmm10,xmm11
  00000001408E2850: addss       xmm11,xmm7
  00000001408E2855: movaps      xmm0,xmm10
  00000001408E2859: subss       xmm0,xmm14
  00000001408E285E: movaps      xmm1,xmm11
  00000001408E2862: shufps      xmm1,xmm0,0
  00000001408E2866: mulps       xmm1,xmmword ptr [rsp+40h]
  00000001408E286B: movaps      xmm0,xmm1
  00000001408E286E: unpckhpd    xmm0,xmm1
  00000001408E2872: addps       xmm0,xmm1
  00000001408E2875: movlps      qword ptr [rsp+28h],xmm0
  00000001408E287A: movups      xmm0,xmmword ptr [rdi+14h]
  00000001408E287E: movaps      xmmword ptr [rsp+80h],xmm0
  00000001408E2886: movss       xmm0,dword ptr [rdi+18h]
  00000001408E288B: movss       dword ptr [rsp+3Ch],xmm0
  00000001408E2891: movss       xmm12,dword ptr [rdi+4Ch]
  00000001408E2897: lea         rcx,[rsp+30h]
  00000001408E289C: lea         rdx,[rsp+28h]
  00000001408E28A1: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001408E28A6: movsd       xmm15,mmword ptr [rsp+30h]
  00000001408E28AD: movss       xmm8,dword ptr [rsp+38h]
  00000001408E28B4: movss       xmm13,dword ptr [rdi+30h]
  00000001408E28BA: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E28C2: ucomiss     xmm0,xmm13
  00000001408E28C6: xorps       xmm6,xmm6
  00000001408E28C9: jae         00000001408E2985
  00000001408E28CF: movshdup    xmm0,xmm9
  00000001408E28D4: movaps      xmm1,xmm9
  00000001408E28D8: cmpunordss  xmm1,xmm9
  00000001408E28DE: movaps      xmm2,xmm1
  00000001408E28E1: andps       xmm2,xmm0
  00000001408E28E4: maxss       xmm0,xmm9
  00000001408E28E9: andnps      xmm1,xmm0
  00000001408E28EC: orps        xmm1,xmm2
  00000001408E28EF: addss       xmm1,dword ptr [__real@c0100000]
  00000001408E28F7: divss       xmm1,dword ptr [__real@414c0000]
  00000001408E28FF: divss       xmm1,dword ptr [__real@3ecccccd]
  00000001408E2907: xorps       xmm2,xmm2
  00000001408E290A: maxss       xmm2,xmm1
  00000001408E290E: movss       xmm0,dword ptr [__real@3f800000]
  00000001408E2916: movaps      xmm1,xmm0
  00000001408E2919: minss       xmm1,xmm2
  00000001408E291D: movaps      xmm2,xmm1
  00000001408E2920: mulss       xmm2,xmm1
  00000001408E2924: addss       xmm1,xmm1
  00000001408E2928: movss       xmm6,dword ptr [__real@40400000]
  00000001408E2930: subss       xmm6,xmm1
  00000001408E2934: mulss       xmm6,xmm2
  00000001408E2938: movaps      xmm2,xmm0
  00000001408E293B: subss       xmm2,xmm6
  00000001408E293F: mulss       xmm2,dword ptr [__real@41100000]
  00000001408E2947: mulss       xmm6,dword ptr [__real@41400000]
  00000001408E294F: xorps       xmm1,xmm1
  00000001408E2952: addss       xmm6,xmm2
  00000001408E2956: ucomiss     xmm6,xmm1
  00000001408E2959: jb          00000001408E2E52
  00000001408E295F: subss       xmm0,xmm13
  00000001408E2964: mulss       xmm0,dword ptr [__real@3ecccccd]
  00000001408E296C: movss       xmm2,dword ptr [__real@41400000]
  00000001408E2974: mulss       xmm2,xmm13
  00000001408E2979: addss       xmm2,xmm0
  00000001408E297D: maxss       xmm1,xmm2
  00000001408E2981: minss       xmm6,xmm1
  00000001408E2985: shufps      xmm11,xmm11,0
  00000001408E298A: lea         rcx,[rsp+40h]
  00000001408E298F: lea         rdx,[rsp+50h]
  00000001408E2994: call        _ZN5utils8geometry9rectangle11Rectangle2d16orientation_mat217h578587259d3307efE
  00000001408E2999: addss       xmm14,xmm10
  00000001408E299E: shufps      xmm11,xmm14,4
  00000001408E29A3: mulps       xmm11,xmmword ptr [rsp+40h]
  00000001408E29A9: movaps      xmm0,xmm11
  00000001408E29AD: unpckhpd    xmm0,xmm11
  00000001408E29B2: addps       xmm0,xmm11
  00000001408E29B6: movlps      qword ptr [rsp+28h],xmm0
  00000001408E29BB: lea         rcx,[rsp+30h]
  00000001408E29C0: lea         rdx,[rsp+28h]
  00000001408E29C5: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001408E29CA: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E29D2: ucomiss     xmm0,xmm13
  00000001408E29D6: movsd       xmm2,mmword ptr [rsp+30h]
  00000001408E29DC: movss       xmm0,dword ptr [rsp+38h]
  00000001408E29E2: xorps       xmm1,xmm1
  00000001408E29E5: jae         00000001408E2D8A
  00000001408E29EB: movshdup    xmm1,xmm9
  00000001408E29F0: movaps      xmm3,xmm9
  00000001408E29F4: cmpunordss  xmm3,xmm9
  00000001408E29FA: movaps      xmm4,xmm3
  00000001408E29FD: andps       xmm4,xmm1
  00000001408E2A00: maxss       xmm1,xmm9
  00000001408E2A05: andnps      xmm3,xmm1
  00000001408E2A08: orps        xmm3,xmm4
  00000001408E2A0B: addss       xmm3,dword ptr [__real@c0100000]
  00000001408E2A13: divss       xmm3,dword ptr [__real@414c0000]
  00000001408E2A1B: divss       xmm3,dword ptr [__real@3ecccccd]
  00000001408E2A23: xorps       xmm1,xmm1
  00000001408E2A26: maxss       xmm1,xmm3
  00000001408E2A2A: movss       xmm3,dword ptr [__real@3f800000]
  00000001408E2A32: movaps      xmm4,xmm3
  00000001408E2A35: minss       xmm4,xmm1
  00000001408E2A39: movaps      xmm5,xmm4
  00000001408E2A3C: mulss       xmm5,xmm4
  00000001408E2A40: addss       xmm4,xmm4
  00000001408E2A44: movss       xmm1,dword ptr [__real@40400000]
  00000001408E2A4C: subss       xmm1,xmm4
  00000001408E2A50: mulss       xmm1,xmm5
  00000001408E2A54: movaps      xmm5,xmm3
  00000001408E2A57: subss       xmm5,xmm1
  00000001408E2A5B: mulss       xmm5,dword ptr [__real@41100000]
  00000001408E2A63: mulss       xmm1,dword ptr [__real@41400000]
  00000001408E2A6B: xorps       xmm4,xmm4
  00000001408E2A6E: addss       xmm1,xmm5
  00000001408E2A72: ucomiss     xmm1,xmm4
  00000001408E2A75: jb          00000001408E2E64
  00000001408E2A7B: subss       xmm3,xmm13
  00000001408E2A80: mulss       xmm3,dword ptr [__real@3ecccccd]
  00000001408E2A88: mulss       xmm13,dword ptr [__real@41400000]
  00000001408E2A91: addss       xmm13,xmm3
  00000001408E2A96: maxss       xmm4,xmm13
  00000001408E2A9B: jmp         00000001408E2D86
  00000001408E2AA0: test        byte ptr [rdi+8],1
  00000001408E2AA4: je          00000001408E2AEF
  00000001408E2AA6: mov         rax,qword ptr [rdi+1Ch]
  00000001408E2AAA: mov         qword ptr [rsp+60h],rax
  00000001408E2AAF: movups      xmm0,xmmword ptr [rdi+0Ch]
  00000001408E2AB3: movaps      xmmword ptr [rsp+50h],xmm0
  00000001408E2AB8: lea         rcx,[rsp+40h]
  00000001408E2ABD: lea         rdx,[rsp+50h]
  00000001408E2AC2: call        _ZN5utils8geometry9rectangle11Rectangle2d16orientation_mat217h578587259d3307efE
  00000001408E2AC7: movsd       xmm0,mmword ptr [rdi+1Ch]
  00000001408E2ACC: movsd       xmm1,mmword ptr [rdi+24h]
  00000001408E2AD1: mulps       xmm1,xmm0
  00000001408E2AD4: unpcklps    xmm1,xmm1
  00000001408E2AD7: mulps       xmm1,xmmword ptr [__xmm@3f0000003f0000003f0000003f000000]
  00000001408E2ADE: mulps       xmm1,xmmword ptr [rsp+40h]
  00000001408E2AE3: movaps      xmm2,xmm1
  00000001408E2AE6: unpckhpd    xmm2,xmm1
  00000001408E2AEA: addps       xmm2,xmm1
  00000001408E2AED: jmp         00000001408E2B0B
  00000001408E2AEF: movss       xmm0,dword ptr [rdi+14h]
  00000001408E2AF4: addss       xmm0,xmm0
  00000001408E2AF8: movsd       xmm1,mmword ptr [rdi+24h]
  00000001408E2AFD: movsldup    xmm2,xmm0
  00000001408E2B01: mulps       xmm2,xmm1
  00000001408E2B04: mulps       xmm2,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001408E2B0B: lea         rcx,[rsi+4]
  00000001408E2B0F: movshdup    xmm3,xmm2
  00000001408E2B13: mov         rdx,rdi
  00000001408E2B16: call        00000001408E2EB0
  00000001408E2B1B: mov         dword ptr [rsi],0
  00000001408E2B21: jmp         00000001408E2DED
  00000001408E2B26: lea         rcx,[rsp+40h]
  00000001408E2B2B: lea         rdx,[rsp+50h]
  00000001408E2B30: call        _ZN5utils8geometry9rectangle11Rectangle2d16orientation_mat217h578587259d3307efE
  00000001408E2B35: mulss       xmm13,dword ptr [__real@3f000000]
  00000001408E2B3E: movaps      xmm10,xmm11
  00000001408E2B42: subps       xmm10,xmm13
  00000001408E2B46: addps       xmm6,xmm11
  00000001408E2B4A: shufps      xmm10,xmm6,50h
  00000001408E2B4F: movaps      xmm0,xmmword ptr [rsp+40h]
  00000001408E2B54: mulps       xmm0,xmm10
  00000001408E2B58: movaps      xmm1,xmm0
  00000001408E2B5B: unpckhpd    xmm1,xmm0
  00000001408E2B5F: addps       xmm1,xmm0
  00000001408E2B62: movlps      qword ptr [rsp+28h],xmm1
  00000001408E2B67: movups      xmm0,xmmword ptr [rdi+14h]
  00000001408E2B6B: movaps      xmmword ptr [rsp+80h],xmm0
  00000001408E2B73: movss       xmm0,dword ptr [rdi+18h]
  00000001408E2B78: movss       dword ptr [rsp+3Ch],xmm0
  00000001408E2B7E: movss       xmm12,dword ptr [rdi+4Ch]
  00000001408E2B84: lea         rcx,[rsp+30h]
  00000001408E2B89: lea         rdx,[rsp+28h]
  00000001408E2B8E: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001408E2B93: movsd       xmm15,mmword ptr [rsp+30h]
  00000001408E2B9A: movss       xmm8,dword ptr [rsp+38h]
  00000001408E2BA1: movss       xmm14,dword ptr [rdi+30h]
  00000001408E2BA7: xorps       xmm7,xmm7
  00000001408E2BAA: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E2BB2: ucomiss     xmm0,xmm14
  00000001408E2BB6: xorps       xmm6,xmm6
  00000001408E2BB9: jae         00000001408E2C75
  00000001408E2BBF: movshdup    xmm0,xmm9
  00000001408E2BC4: movaps      xmm1,xmm9
  00000001408E2BC8: cmpunordss  xmm1,xmm9
  00000001408E2BCE: movaps      xmm2,xmm1
  00000001408E2BD1: andps       xmm2,xmm0
  00000001408E2BD4: maxss       xmm0,xmm9
  00000001408E2BD9: andnps      xmm1,xmm0
  00000001408E2BDC: orps        xmm1,xmm2
  00000001408E2BDF: addss       xmm1,dword ptr [__real@c0100000]
  00000001408E2BE7: divss       xmm1,dword ptr [__real@414c0000]
  00000001408E2BEF: divss       xmm1,dword ptr [__real@3ecccccd]
  00000001408E2BF7: xorps       xmm2,xmm2
  00000001408E2BFA: maxss       xmm2,xmm1
  00000001408E2BFE: movss       xmm0,dword ptr [__real@3f800000]
  00000001408E2C06: movaps      xmm1,xmm0
  00000001408E2C09: minss       xmm1,xmm2
  00000001408E2C0D: movaps      xmm2,xmm1
  00000001408E2C10: mulss       xmm2,xmm1
  00000001408E2C14: addss       xmm1,xmm1
  00000001408E2C18: movss       xmm6,dword ptr [__real@40400000]
  00000001408E2C20: subss       xmm6,xmm1
  00000001408E2C24: mulss       xmm6,xmm2
  00000001408E2C28: movaps      xmm2,xmm0
  00000001408E2C2B: subss       xmm2,xmm6
  00000001408E2C2F: mulss       xmm2,dword ptr [__real@41100000]
  00000001408E2C37: mulss       xmm6,dword ptr [__real@41400000]
  00000001408E2C3F: xorps       xmm1,xmm1
  00000001408E2C42: addss       xmm6,xmm2
  00000001408E2C46: ucomiss     xmm6,xmm1
  00000001408E2C49: jb          00000001408E2E52
  00000001408E2C4F: subss       xmm0,xmm14
  00000001408E2C54: mulss       xmm0,dword ptr [__real@3ecccccd]
  00000001408E2C5C: movss       xmm2,dword ptr [__real@41400000]
  00000001408E2C64: mulss       xmm2,xmm14
  00000001408E2C69: addss       xmm2,xmm0
  00000001408E2C6D: maxss       xmm1,xmm2
  00000001408E2C71: minss       xmm6,xmm1
  00000001408E2C75: lea         rcx,[rsp+40h]
  00000001408E2C7A: lea         rdx,[rsp+50h]
  00000001408E2C7F: call        _ZN5utils8geometry9rectangle11Rectangle2d16orientation_mat217h578587259d3307efE
  00000001408E2C84: addss       xmm13,xmm11
  00000001408E2C89: shufps      xmm13,xmm10,0A0h
  00000001408E2C8E: mulps       xmm13,xmmword ptr [rsp+40h]
  00000001408E2C94: movaps      xmm0,xmm13
  00000001408E2C98: unpckhpd    xmm0,xmm13
  00000001408E2C9D: addps       xmm0,xmm13
  00000001408E2CA1: movlps      qword ptr [rsp+28h],xmm0
  00000001408E2CA6: lea         rcx,[rsp+30h]
  00000001408E2CAB: lea         rdx,[rsp+28h]
  00000001408E2CB0: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001408E2CB5: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E2CBD: ucomiss     xmm0,xmm14
  00000001408E2CC1: movsd       xmm2,mmword ptr [rsp+30h]
  00000001408E2CC7: movss       xmm0,dword ptr [rsp+38h]
  00000001408E2CCD: xorps       xmm1,xmm1
  00000001408E2CD0: jae         00000001408E2D8A
  00000001408E2CD6: movshdup    xmm1,xmm9
  00000001408E2CDB: movaps      xmm3,xmm9
  00000001408E2CDF: cmpunordss  xmm3,xmm9
  00000001408E2CE5: movaps      xmm4,xmm3
  00000001408E2CE8: andps       xmm4,xmm1
  00000001408E2CEB: maxss       xmm1,xmm9
  00000001408E2CF0: andnps      xmm3,xmm1
  00000001408E2CF3: orps        xmm3,xmm4
  00000001408E2CF6: addss       xmm3,dword ptr [__real@c0100000]
  00000001408E2CFE: divss       xmm3,dword ptr [__real@414c0000]
  00000001408E2D06: divss       xmm3,dword ptr [__real@3ecccccd]
  00000001408E2D0E: xorps       xmm1,xmm1
  00000001408E2D11: maxss       xmm1,xmm3
  00000001408E2D15: movss       xmm3,dword ptr [__real@3f800000]
  00000001408E2D1D: movaps      xmm4,xmm3
  00000001408E2D20: minss       xmm4,xmm1
  00000001408E2D24: movaps      xmm5,xmm4
  00000001408E2D27: mulss       xmm5,xmm4
  00000001408E2D2B: addss       xmm4,xmm4
  00000001408E2D2F: movss       xmm1,dword ptr [__real@40400000]
  00000001408E2D37: subss       xmm1,xmm4
  00000001408E2D3B: mulss       xmm1,xmm5
  00000001408E2D3F: movaps      xmm5,xmm3
  00000001408E2D42: subss       xmm5,xmm1
  00000001408E2D46: mulss       xmm5,dword ptr [__real@41100000]
  00000001408E2D4E: mulss       xmm1,dword ptr [__real@41400000]
  00000001408E2D56: xorps       xmm4,xmm4
  00000001408E2D59: addss       xmm1,xmm5
  00000001408E2D5D: ucomiss     xmm1,xmm4
  00000001408E2D60: jb          00000001408E2E64
  00000001408E2D66: subss       xmm3,xmm14
  00000001408E2D6B: mulss       xmm3,dword ptr [__real@3ecccccd]
  00000001408E2D73: mulss       xmm14,dword ptr [__real@41400000]
  00000001408E2D7C: addss       xmm14,xmm3
  00000001408E2D81: maxss       xmm4,xmm14
  00000001408E2D86: minss       xmm1,xmm4
  00000001408E2D8A: movss       xmm3,dword ptr [rsp+3Ch]
  00000001408E2D90: addss       xmm8,xmm3
  00000001408E2D95: movaps      xmm4,xmmword ptr [rsp+80h]
  00000001408E2D9D: movlhps     xmm12,xmm4
  00000001408E2DA1: shufps      xmm12,xmm4,0E2h
  00000001408E2DA6: addps       xmm15,xmm12
  00000001408E2DAA: mulss       xmm7,xmm6
  00000001408E2DAE: addss       xmm8,xmm7
  00000001408E2DB3: unpcklps    xmm7,xmm6
  00000001408E2DB6: addps       xmm7,xmm15
  00000001408E2DBA: addss       xmm0,xmm3
  00000001408E2DBE: addps       xmm12,xmm2
  00000001408E2DC2: xorps       xmm2,xmm2
  00000001408E2DC5: mulss       xmm2,xmm1
  00000001408E2DC9: addss       xmm0,xmm2
  00000001408E2DCD: unpcklps    xmm2,xmm1
  00000001408E2DD0: addps       xmm2,xmm12
  00000001408E2DD4: movlps      qword ptr [rsi+4],xmm7
  00000001408E2DD8: movss       dword ptr [rsi+0Ch],xmm8
  00000001408E2DDE: movlps      qword ptr [rsi+10h],xmm2
  00000001408E2DE2: movss       dword ptr [rsi+18h],xmm0
  00000001408E2DE7: mov         dword ptr [rsi],1
  00000001408E2DED: mov         rax,rsi
  00000001408E2DF0: movaps      xmm6,xmmword ptr [rsp+90h]
  00000001408E2DF8: movaps      xmm7,xmmword ptr [rsp+0A0h]
  00000001408E2E00: movaps      xmm8,xmmword ptr [rsp+0B0h]
  00000001408E2E09: movaps      xmm9,xmmword ptr [rsp+0C0h]
  00000001408E2E12: movaps      xmm10,xmmword ptr [rsp+0D0h]
  00000001408E2E1B: movaps      xmm11,xmmword ptr [rsp+0E0h]
  00000001408E2E24: movaps      xmm12,xmmword ptr [rsp+0F0h]
  00000001408E2E2D: movaps      xmm13,xmmword ptr [rsp+100h]
  00000001408E2E36: movaps      xmm14,xmmword ptr [rsp+110h]
  00000001408E2E3F: movaps      xmm15,xmmword ptr [rsp+120h]
  00000001408E2E48: add         rsp,138h
  00000001408E2E4F: pop         rdi
  00000001408E2E50: pop         rsi
  00000001408E2E51: ret
  00000001408E2E52: lea         r8,[anon.12ce4fde5425384d5f438f5122902e1a.4.llvm.3277990281340110158]
  00000001408E2E59: xorps       xmm0,xmm0
  00000001408E2E5C: movaps      xmm1,xmm6
  00000001408E2E5F: call        _ZN4core3f3221_$LT$impl$u20$f32$GT$5clamp8do_panic7runtime17h74ed5b95db14622dE
  00000001408E2E64: lea         r8,[anon.12ce4fde5425384d5f438f5122902e1a.4.llvm.3277990281340110158]
  00000001408E2E6B: xorps       xmm0,xmm0
  00000001408E2E6E: call        _ZN4core3f3221_$LT$impl$u20$f32$GT$5clamp8do_panic7runtime17h74ed5b95db14622dE
  00000001408E2E73: lea         rax,[142AA57D0h]
  00000001408E2E7A: mov         qword ptr [rsp+50h],rax
  00000001408E2E7F: mov         qword ptr [rsp+58h],1
  00000001408E2E88: mov         qword ptr [rsp+60h],8
  00000001408E2E91: xorps       xmm0,xmm0
  00000001408E2E94: movups      xmmword ptr [rsp+68h],xmm0
  00000001408E2E99: lea         rdx,[142AA5818h]
  00000001408E2EA0: lea         rcx,[rsp+50h]
  00000001408E2EA5: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  00000001408E2EAA: int         3

; ==================================================================
; roof_ridge::calc_roof_tip_center_ls / inverse_ridge_01_param（全文）
; _ZN12country_core9resources5roofs10roof_ridge23calc_roof_tip_center_ls17h41f7de4b1f1b8a19E:
  00000001408E30B0: cmp         byte ptr [rcx+8],0
  00000001408E30B4: jne         00000001408E30C4
  00000001408E30B6: movss       xmm0,dword ptr [rcx+14h]
  00000001408E30BB: addss       xmm0,xmm0
  00000001408E30BF: movaps      xmm3,xmm0
  00000001408E30C2: jmp         00000001408E30CE
  00000001408E30C4: movss       xmm0,dword ptr [rcx+1Ch]
  00000001408E30C9: movss       xmm3,dword ptr [rcx+20h]
  00000001408E30CE: mulss       xmm0,dword ptr [rcx+24h]
  00000001408E30D3: mulss       xmm3,dword ptr [rcx+28h]
  00000001408E30D8: mulss       xmm0,xmm1
  00000001408E30DC: mulss       xmm3,xmm2
  00000001408E30E0: movaps      xmm1,xmm3
  00000001408E30E3: ret
  00000001408E30E4: CC CC CC CC CC CC CC CC CC CC CC CC              ............
; _ZN12country_core9resources5roofs10roof_ridge22inverse_ridge_01_param17hac279c2a78f86561E:
  00000001408E30F0: sub         rsp,28h
  00000001408E30F4: test        dl,dl
  00000001408E30F6: jne         00000001408E30FB
  00000001408E30F8: movaps      xmm3,xmm2
  00000001408E30FB: ucomiss     xmm3,dword ptr [__real@3dcccccd]
  00000001408E3102: jne         00000001408E310A
  00000001408E3104: jnp         00000001408E3193
  00000001408E310A: movss       xmm1,dword ptr [__real@bdcccccd]
  00000001408E3112: addss       xmm0,xmm1
  00000001408E3116: addss       xmm3,xmm1
  00000001408E311A: divss       xmm0,xmm3
  00000001408E311E: xorps       xmm2,xmm2
  00000001408E3121: ucomiss     xmm2,xmm0
  00000001408E3124: seta        al
  00000001408E3127: maxss       xmm2,xmm0
  00000001408E312B: movss       xmm1,dword ptr [__real@3f800000]
  00000001408E3133: ucomiss     xmm2,xmm1
  00000001408E3136: movd        ecx,xmm0
  00000001408E313A: seta        dl
  00000001408E313D: and         ecx,7FFFFFFFh
  00000001408E3143: cmp         ecx,7F800000h
  00000001408E3149: setl        cl
  00000001408E314C: or          cl,al
  00000001408E314E: or          cl,dl
  00000001408E3150: je          00000001408E3163
  00000001408E3152: minss       xmm1,xmm2
  00000001408E3156: ucomiss     xmm1,xmm1
  00000001408E3159: jp          00000001408E317B
  00000001408E315B: movaps      xmm0,xmm1
  00000001408E315E: add         rsp,28h
  00000001408E3162: ret
  00000001408E3163: lea         rcx,[142AA5848h]
  00000001408E316A: lea         r8,[142AA5878h]
  00000001408E3171: mov         edx,29h
  00000001408E3176: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001408E317B: lea         rcx,[142AA5890h]
  00000001408E3182: lea         r8,[142AA58B8h]
  00000001408E3189: mov         edx,27h
  00000001408E318E: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  00000001408E3193: lea         rcx,[142AA5830h]
  00000001408E319A: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  00000001408E319F: int         3

; ==================================================================
; roof_shape::Roof::ty / height / is_flat / is_flat_w_no_rim / is_gable / gable_sides（全文）
; _ZN12country_core9resources5roofs10roof_shape4Roof2ty17h001241e6e27ea1b3E:
  00000001408E3250: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E3258: ucomiss     xmm0,dword ptr [rcx+30h]
  00000001408E325C: setb        al
  00000001408E325F: ret
; _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE:
  00000001408E3260: sub         rsp,38h
  00000001408E3264: movaps      xmmword ptr [rsp+20h],xmm6
  00000001408E3269: movss       xmm0,dword ptr [rcx+30h]
  00000001408E326E: xorps       xmm1,xmm1
  00000001408E3271: movss       xmm2,dword ptr [__real@3dcccccd]
  00000001408E3279: ucomiss     xmm2,xmm0
  00000001408E327C: jae         00000001408E3359
  00000001408E3282: test        byte ptr [rcx+8],1
  00000001408E3286: je          00000001408E32C4
  00000001408E3288: movss       xmm2,dword ptr [rcx+1Ch]
  00000001408E328D: movss       xmm3,dword ptr [rcx+20h]
  00000001408E3292: movaps      xmm1,xmm2
  00000001408E3295: cmpunordss  xmm1,xmm2
  00000001408E329A: movaps      xmm4,xmm1
  00000001408E329D: andps       xmm4,xmm3
  00000001408E32A0: maxss       xmm3,xmm2
  00000001408E32A4: andnps      xmm1,xmm3
  00000001408E32A7: orps        xmm1,xmm4
  00000001408E32AA: addss       xmm1,dword ptr [__real@c0100000]
  00000001408E32B2: divss       xmm1,dword ptr [__real@414c0000]
  00000001408E32BA: movss       xmm3,dword ptr [__real@41100000]
  00000001408E32C2: jmp         00000001408E32E1
  00000001408E32C4: movss       xmm1,dword ptr [rcx+14h]
  00000001408E32C9: addss       xmm1,dword ptr [__real@bf666667]
  00000001408E32D1: divss       xmm1,dword ptr [__real@40e33333]
  00000001408E32D9: movss       xmm3,dword ptr [__real@41000000]
  00000001408E32E1: divss       xmm1,dword ptr [__real@3ecccccd]
  00000001408E32E9: xorps       xmm2,xmm2
  00000001408E32EC: xorps       xmm5,xmm5
  00000001408E32EF: maxss       xmm5,xmm1
  00000001408E32F3: movss       xmm4,dword ptr [__real@3f800000]
  00000001408E32FB: movaps      xmm6,xmm4
  00000001408E32FE: minss       xmm6,xmm5
  00000001408E3302: movaps      xmm5,xmm6
  00000001408E3305: mulss       xmm5,xmm6
  00000001408E3309: addss       xmm6,xmm6
  00000001408E330D: movss       xmm1,dword ptr [__real@40400000]
  00000001408E3315: subss       xmm1,xmm6
  00000001408E3319: mulss       xmm1,xmm5
  00000001408E331D: movaps      xmm5,xmm4
  00000001408E3320: subss       xmm5,xmm1
  00000001408E3324: mulss       xmm1,dword ptr [__real@41400000]
  00000001408E332C: mulss       xmm5,xmm3
  00000001408E3330: addss       xmm1,xmm5
  00000001408E3334: ucomiss     xmm1,xmm2
  00000001408E3337: jb          00000001408E3366
  00000001408E3339: subss       xmm4,xmm0
  00000001408E333D: mulss       xmm4,dword ptr [__real@3ecccccd]
  00000001408E3345: mulss       xmm0,dword ptr [__real@41400000]
  00000001408E334D: addss       xmm0,xmm4
  00000001408E3351: maxss       xmm2,xmm0
  00000001408E3355: minss       xmm1,xmm2
  00000001408E3359: movaps      xmm0,xmm1
  00000001408E335C: movaps      xmm6,xmmword ptr [rsp+20h]
  00000001408E3361: add         rsp,38h
  00000001408E3365: ret
  00000001408E3366: lea         r8,[anon.12ce4fde5425384d5f438f5122902e1a.4.llvm.3277990281340110158]
  00000001408E336D: xorps       xmm0,xmm0
  00000001408E3370: call        _ZN4core3f3221_$LT$impl$u20$f32$GT$5clamp8do_panic7runtime17h74ed5b95db14622dE
  00000001408E3375: int         3
  00000001408E3376: CC CC CC CC CC CC CC CC CC CC                    ..........
; _ZN12country_core9resources5roofs10roof_shape4Roof7is_flat17hf1ba4121fb5a18d9E:
  00000001408E3380: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E3388: ucomiss     xmm0,dword ptr [rcx+30h]
  00000001408E338C: setae       al
  00000001408E338F: ret
; _ZN12country_core9resources5roofs10roof_shape4Roof16is_flat_w_no_rim17h21bdec18793fdd7eE:
  00000001408E3390: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E3398: ucomiss     xmm0,dword ptr [rcx+30h]
  00000001408E339C: setae       dl
  00000001408E339F: cmp         byte ptr [rcx+3Dh],0
  00000001408E33A3: setne       al
  00000001408E33A6: and         al,dl
  00000001408E33A8: ret
  00000001408E33A9: CC CC CC CC CC CC CC                             .......
; _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E:
  00000001408E33B0: movss       xmm0,dword ptr [rcx+30h]
  00000001408E33B5: movss       xmm1,dword ptr [rcx+34h]
  00000001408E33BA: movss       xmm2,dword ptr [__real@3f800000]
  00000001408E33C2: cmpeqps     xmm2,xmm1
  00000001408E33C6: movss       xmm1,dword ptr [__real@3dcccccd]
  00000001408E33CE: cmpnleps    xmm0,xmm1
  00000001408E33D2: andps       xmm0,xmm2
  00000001408E33D5: movd        eax,xmm0
  00000001408E33D9: and         al,1
  00000001408E33DB: ret
  00000001408E33DC: CC CC CC CC                                      ....
; _ZN12country_core9resources5roofs10roof_shape4Roof11gable_sides17h7e1ab50eb9948917E:
  00000001408E33E0: mov         rax,rcx
  00000001408E33E3: movss       xmm0,dword ptr [rdx+34h]
  00000001408E33E8: xor         ecx,ecx
  00000001408E33EA: ucomiss     xmm0,dword ptr [__real@3f800000]
  00000001408E33F1: jne         00000001408E3418
  00000001408E33F3: jp          00000001408E3418
  00000001408E33F5: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E33FD: ucomiss     xmm0,dword ptr [rdx+30h]
  00000001408E3401: jae         00000001408E3418
  00000001408E3403: movzx       ecx,byte ptr [rdx+3Ch]
  00000001408E3407: mov         qword ptr [rax+8],rcx
  00000001408E340B: or          rcx,2
  00000001408E340F: mov         qword ptr [rax+10h],rcx
  00000001408E3413: mov         ecx,1
  00000001408E3418: mov         qword ptr [rax],rcx
  00000001408E341B: ret

; ==================================================================
; roof_shape::ridge_length_01_from_rectangle_ratio / RoofShapeParams::eave_length_ws / roof_height_ws_from_wall_shape（全文）
; _ZN12country_core9resources5roofs10roof_shape36ridge_length_01_from_rectangle_ratio17hefb06c6f636faff2E:
  00000001408E3B60: sub         rsp,18h
  00000001408E3B64: movaps      xmmword ptr [rsp],xmm6
  00000001408E3B68: movaps      xmm3,xmm0
  00000001408E3B6B: divss       xmm3,xmm1
  00000001408E3B6F: movaps      xmm4,xmm1
  00000001408E3B72: divss       xmm4,xmm0
  00000001408E3B76: movaps      xmm2,xmm3
  00000001408E3B79: cmpunordss  xmm2,xmm3
  00000001408E3B7E: movaps      xmm5,xmm2
  00000001408E3B81: andps       xmm5,xmm4
  00000001408E3B84: maxss       xmm4,xmm3
  00000001408E3B88: andnps      xmm2,xmm4
  00000001408E3B8B: movaps      xmm3,xmm0
  00000001408E3B8E: cmpunordss  xmm3,xmm0
  00000001408E3B93: movaps      xmm4,xmm3
  00000001408E3B96: andps       xmm4,xmm1
  00000001408E3B99: maxss       xmm1,xmm0
  00000001408E3B9D: andnps      xmm3,xmm1
  00000001408E3BA0: orps        xmm3,xmm4
  00000001408E3BA3: addss       xmm3,dword ptr [__real@c0100000]
  00000001408E3BAB: divss       xmm3,dword ptr [__real@40a80000]
  00000001408E3BB3: orps        xmm2,xmm5
  00000001408E3BB6: xorps       xmm1,xmm1
  00000001408E3BB9: xorps       xmm0,xmm0
  00000001408E3BBC: maxss       xmm0,xmm3
  00000001408E3BC0: movss       xmm3,dword ptr [__real@3f800000]
  00000001408E3BC8: movaps      xmm4,xmm3
  00000001408E3BCB: minss       xmm4,xmm0
  00000001408E3BCF: movaps      xmm5,xmm4
  00000001408E3BD2: mulss       xmm5,xmm4
  00000001408E3BD6: addss       xmm4,xmm4
  00000001408E3BDA: movss       xmm0,dword ptr [__real@40400000]
  00000001408E3BE2: movaps      xmm6,xmm0
  00000001408E3BE5: subss       xmm6,xmm4
  00000001408E3BE9: mulss       xmm6,xmm5
  00000001408E3BED: movss       xmm4,dword ptr [__real@3fc00000]
  00000001408E3BF5: mulss       xmm6,xmm4
  00000001408E3BF9: addss       xmm6,xmm2
  00000001408E3BFD: addss       xmm6,dword ptr [__real@bfc00000]
  00000001408E3C05: divss       xmm6,xmm4
  00000001408E3C09: maxss       xmm1,xmm6
  00000001408E3C0D: minss       xmm3,xmm1
  00000001408E3C11: movaps      xmm1,xmm3
  00000001408E3C14: mulss       xmm1,xmm3
  00000001408E3C18: addss       xmm3,xmm3
  00000001408E3C1C: subss       xmm0,xmm3
  00000001408E3C20: mulss       xmm0,xmm1
  00000001408E3C24: movaps      xmm6,xmmword ptr [rsp]
  00000001408E3C28: add         rsp,18h
  00000001408E3C2C: ret
  00000001408E3C2D: CC CC CC                                         ...
; _ZN12country_core9resources5roofs10roof_shape15RoofShapeParams14eave_length_ws17h40916087298468ffE:
  00000001408E3C30: movss       xmm0,dword ptr [rcx+8]
  00000001408E3C35: movss       xmm1,dword ptr [rcx+14h]
  00000001408E3C3A: movss       xmm2,dword ptr [__real@3fc00000]
  00000001408E3C42: mulss       xmm2,xmm1
  00000001408E3C46: sqrtss      xmm0,xmm0
  00000001408E3C4A: unpcklps    xmm1,xmm0
  00000001408E3C4D: movaps      xmm0,xmmword ptr [__xmm@00000000000000003f8000003f800000]
  00000001408E3C54: subps       xmm0,xmm1
  00000001408E3C57: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f00000000000000]
  00000001408E3C5E: movss       xmm1,xmm2
  00000001408E3C62: addps       xmm1,xmm0
  00000001408E3C65: movshdup    xmm0,xmm1
  00000001408E3C69: mulss       xmm0,xmm1
  00000001408E3C6D: ret
  00000001408E3C6E: CC CC                                            ..
; _ZN12country_core9resources5roofs10roof_shape30roof_height_ws_from_wall_shape17hbc588bde5238e07eE:
  00000001408E3C70: sub         rsp,48h
  00000001408E3C74: movaps      xmmword ptr [rsp+30h],xmm7
  00000001408E3C79: movaps      xmmword ptr [rsp+20h],xmm6
  00000001408E3C7E: test        byte ptr [rcx],1
  00000001408E3C81: je          00000001408E3CC7
  00000001408E3C83: movss       xmm0,dword ptr [rcx+14h]
  00000001408E3C88: movss       xmm2,dword ptr [rcx+18h]
  00000001408E3C8D: movaps      xmm1,xmm0
  00000001408E3C90: cmpunordss  xmm1,xmm0
  00000001408E3C95: movaps      xmm3,xmm1
  00000001408E3C98: andps       xmm3,xmm2
  00000001408E3C9B: maxss       xmm2,xmm0
  00000001408E3C9F: andnps      xmm1,xmm2
  00000001408E3CA2: orps        xmm1,xmm3
  00000001408E3CA5: addss       xmm1,dword ptr [__real@c0100000]
  00000001408E3CAD: divss       xmm1,dword ptr [__real@414c0000]
  00000001408E3CB5: movss       xmm6,dword ptr [__real@40c00000]
  00000001408E3CBD: movss       xmm7,dword ptr [__real@40400000]
  00000001408E3CC5: jmp         00000001408E3CEC
  00000001408E3CC7: movss       xmm1,dword ptr [rcx+0Ch]
  00000001408E3CCC: addss       xmm1,dword ptr [__real@bf666667]
  00000001408E3CD4: divss       xmm1,dword ptr [__real@40e33333]
  00000001408E3CDC: movss       xmm6,dword ptr [__real@41200000]
  00000001408E3CE4: movss       xmm7,dword ptr [__real@40800000]
  00000001408E3CEC: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.48.llvm.3277990281340110158]
  00000001408E3CF3: call        _ZN5utils16easing_functions14EasingFunction5apply17hb692303304673b5bE
  00000001408E3CF8: movss       xmm1,dword ptr [__real@3f800000]
  00000001408E3D00: movaps      xmm2,xmm1
  00000001408E3D03: subss       xmm2,xmm0
  00000001408E3D07: mulss       xmm2,xmm7
  00000001408E3D0B: mulss       xmm6,xmm0
  00000001408E3D0F: addss       xmm6,xmm2
  00000001408E3D13: movss       xmm0,dword ptr [__real@41400000]
  00000001408E3D1B: divss       xmm6,xmm0
  00000001408E3D1F: xorps       xmm2,xmm2
  00000001408E3D22: maxss       xmm2,xmm6
  00000001408E3D26: minss       xmm1,xmm2
  00000001408E3D2A: mulss       xmm1,xmm0
  00000001408E3D2E: movaps      xmm0,xmm1
  00000001408E3D31: movaps      xmm6,xmmword ptr [rsp+20h]
  00000001408E3D36: movaps      xmm7,xmmword ptr [rsp+30h]
  00000001408E3D3B: add         rsp,48h
  00000001408E3D3F: ret

; ==================================================================
; roof_visual_shape::get_roof_expanded_top_and_bottom（全文）
; _ZN12country_core9resources5roofs17roof_visual_shape32get_roof_expanded_top_and_bottom17h49013d2de44c42a5E:
  0000000140A880C0: mov         rax,rcx
  0000000140A880C3: test        byte ptr [rdx+8],1
  0000000140A880C7: je          0000000140A8815E
  0000000140A880CD: movss       xmm1,dword ptr [rdx+38h]
  0000000140A880D2: movss       xmm0,dword ptr [__real@3fc00000]
  0000000140A880DA: mulss       xmm0,xmm1
  0000000140A880DE: movss       xmm2,dword ptr [rdx+2Ch]
  0000000140A880E3: sqrtss      xmm2,xmm2
  0000000140A880E7: unpcklps    xmm1,xmm2
  0000000140A880EA: movaps      xmm2,xmmword ptr [__xmm@00000000000000003f8000003f800000]
  0000000140A880F1: subps       xmm2,xmm1
  0000000140A880F4: mulps       xmm2,xmmword ptr [__xmm@00000000000000003f00000000000000]
  0000000140A880FB: movss       xmm1,xmm0
  0000000140A880FF: addps       xmm1,xmm2
  0000000140A88102: movsd       xmm2,mmword ptr [rdx+1Ch]
  0000000140A88107: movaps      xmm0,xmmword ptr [__xmm@00000000000000003e8f5c293e8f5c29]
  0000000140A8810E: addps       xmm0,xmm2
  0000000140A88111: movshdup    xmm3,xmm1
  0000000140A88115: mulss       xmm3,xmm1
  0000000140A88119: movsldup    xmm1,xmm3
  0000000140A8811D: movss       xmm4,dword ptr [rdx+34h]
  0000000140A88122: movss       xmm3,dword ptr [__real@3f800000]
  0000000140A8812A: subss       xmm3,xmm4
  0000000140A8812E: mulss       xmm3,dword ptr [__real@3dcccccd]
  0000000140A88136: cmp         byte ptr [rdx+3Ch],0
  0000000140A8813A: je          0000000140A881CC
  0000000140A88140: movshdup    xmm2,xmm2
  0000000140A88144: mulss       xmm2,xmm4
  0000000140A88148: addss       xmm2,xmm3
  0000000140A8814C: movd        ecx,xmm2
  0000000140A88150: shl         rcx,20h
  0000000140A88154: mov         edx,3DCCCCCDh
  0000000140A88159: jmp         0000000140A881E2
  0000000140A8815E: movss       xmm0,dword ptr [rdx+2Ch]
  0000000140A88163: movss       xmm1,dword ptr [rdx+38h]
  0000000140A88168: movss       xmm2,dword ptr [__real@3fc00000]
  0000000140A88170: mulss       xmm2,xmm1
  0000000140A88174: sqrtss      xmm0,xmm0
  0000000140A88178: unpcklps    xmm1,xmm0
  0000000140A8817B: movaps      xmm0,xmmword ptr [__xmm@00000000000000003f8000003f800000]
  0000000140A88182: subps       xmm0,xmm1
  0000000140A88185: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f00000000000000]
  0000000140A8818C: movss       xmm1,xmm2
  0000000140A88190: addps       xmm1,xmm0
  0000000140A88193: movshdup    xmm2,xmm1
  0000000140A88197: mulss       xmm2,xmm1
  0000000140A8819B: mulss       xmm2,dword ptr [__real@3f99999a]
  0000000140A881A3: addss       xmm2,dword ptr [__real@3e8f5c29]
  0000000140A881AB: mulss       xmm2,dword ptr [__real@3f000000]
  0000000140A881B3: addss       xmm2,dword ptr [rdx+14h]
  0000000140A881B8: movaps      xmm0,xmmword ptr [__xmm@00000000000000003e4ccccd00000000]
  0000000140A881BF: movss       xmm0,xmm2
  0000000140A881C3: xor         ecx,ecx
  0000000140A881C5: movlps      qword ptr [rax+4],xmm0
  0000000140A881C9: mov         dword ptr [rax],ecx
  0000000140A881CB: ret
  0000000140A881CC: mulss       xmm2,xmm4
  0000000140A881D0: addss       xmm2,xmm3
  0000000140A881D4: movd        edx,xmm2
  0000000140A881D8: mov         rcx,3DCCCCCD00000000h
  0000000140A881E2: or          rcx,rdx
  0000000140A881E5: addps       xmm0,xmm1
  0000000140A881E8: mov         qword ptr [rax+0Ch],rcx
  0000000140A881EC: mov         ecx,1
  0000000140A881F1: movlps      qword ptr [rax+4],xmm0
  0000000140A881F5: mov         dword ptr [rax],ecx
  0000000140A881F7: ret

; ==================================================================
; system_roof::utils::assemble_rectangular_roof —— 头部：檐口 / 脊两个矩形与山墙的顺脊长改写
; _ZN11system_roof5utils25assemble_rectangular_roof25assemble_rectangular_roof17hb107ea4b47d5a245E:
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

; ==================================================================
; assemble_rectangular_roof —— 山墙时水平屋脊盖板的两端
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

; ==================================================================
; assemble_rectangular_roof —— 坡面循环里跳过 gable_sides 的两个面
  00000001421B0B96: mov         r9,qword ptr [rbp+3C0h]
  00000001421B0B9D: cmp         qword ptr [rbp+0E8h],r9
  00000001421B0BA4: je          00000001421B0A10
  00000001421B0BAA: cmp         qword ptr [rbp+0F0h],r9
  00000001421B0BB1: je          00000001421B0A10
  00000001421B0BB7: jmp         00000001421B0C05

; ==================================================================
; assemble_rectangular_roof —— 每个坡面的两张逐瓦裁剪面（过角线的竖直面，向面内 0.075 m）
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

; ==================================================================
; system_roof::create_roof —— 山墙样式字节 = WallStyle::is_halftimbered；是山墙就发 RecalculateGableRoofWallHeights
  00000001421AB0F6: mov         ecx,dword ptr [rax+258h]
  00000001421AB0FC: cmp         ecx,2
  00000001421AB0FF: jae         00000001421AB551
  00000001421AB105: mov         rdx,qword ptr [rax+26Ch]
  00000001421AB10C: lea         r8,[rbp+160h]
  00000001421AB113: mov         qword ptr [r8+10h],rdx
  00000001421AB117: movups      xmm0,xmmword ptr [rax+25Ch]
  00000001421AB11E: movups      xmmword ptr [r8],xmm0
  00000001421AB122: movzx       eax,byte ptr [rax+299h]
  00000001421AB129: mov         dword ptr [rbp+15Ch],ecx
  00000001421AB12F: mov         byte ptr [rbp+2A7h],al
  00000001421AB135: mov         rcx,rbx
  00000001421AB138: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState5max_y17hf214ae5cdebb697aE
  00000001421AB13D: movaps      xmm6,xmm0
  00000001421AB140: mov         rbx,qword ptr [r12+8]
  00000001421AB145: movups      xmm0,xmmword ptr [r12+20h]
  00000001421AB14B: movups      xmm1,xmmword ptr [r12+2Ch]
  00000001421AB151: movups      xmmword ptr [rbp+1DCh],xmm1
  00000001421AB158: movaps      xmmword ptr [rbp+1D0h],xmm0
  00000001421AB15F: mov         r14d,dword ptr [r12+10h]
  00000001421AB164: lea         rcx,[rbp+2A7h]
  00000001421AB16B: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  00000001421AB170: mov         ecx,dword ptr [r12+1Ch]
  00000001421AB175: mov         dword ptr [rbp+180h],ecx
  00000001421AB17B: mov         rcx,qword ptr [r12+14h]
  00000001421AB180: mov         qword ptr [rbp+178h],rcx
  00000001421AB187: movups      xmm0,xmmword ptr [rbp+15Ch]
  00000001421AB18E: movups      xmm1,xmmword ptr [rbp+168h]
  00000001421AB195: movups      xmmword ptr [rbp-34h],xmm1
  00000001421AB199: movaps      xmmword ptr [rbp-40h],xmm0
  00000001421AB19D: lea         rcx,[rbp+178h]
  00000001421AB1A4: mov         qword ptr [rsp+38h],rcx
  00000001421AB1A9: mov         byte ptr [rsp+30h],al
  00000001421AB1AD: mov         dword ptr [rsp+28h],r14d
  00000001421AB1B2: movss       dword ptr [rsp+20h],xmm6
  00000001421AB1B8: lea         r14,[rbp+0E8h]
  00000001421AB1BF: mov         rcx,r14
  00000001421AB1C2: mov         rdx,rbx
  00000001421AB1C5: lea         r8,[rbp+1D0h]
  00000001421AB1CC: lea         r9,[rbp-40h]
  00000001421AB1D0: call        _ZN12country_core9resources5roofs10roof_shape4Roof3new17he6151c7a9c94cf29E
  00000001421AB1D5: mov         rcx,r14
  00000001421AB1D8: call        _ZN12country_core9resources5roofs10roof_shape4Roof2ty17h001241e6e27ea1b3E
  00000001421AB1DD: mov         ebx,eax
  00000001421AB1DF: mov         rcx,r14
  00000001421AB1E2: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  00000001421AB1E7: test        al,al
  00000001421AB1E9: je          00000001421AB26A
  00000001421AB1EB: mov         r13,rsi
  00000001421AB1EE: mov         esi,ebx
  00000001421AB1F0: mov         edi,r15d
  00000001421AB1F3: mov         r15,qword ptr [rbp+0E8h]
  00000001421AB1FA: mov         eax,dword ptr [rbp+2A0h]
  00000001421AB200: mov         rcx,qword ptr [rbp+250h]
  00000001421AB207: mov         dword ptr [rcx],eax
  00000001421AB209: mov         rax,qword ptr [rbp+290h]
  00000001421AB210: mov         r14,qword ptr [rax+30h]
  00000001421AB214: mov         rbx,qword ptr [rax+40h]
  00000001421AB218: cmp         r14,qword ptr [rax+20h]
  00000001421AB21C: jne         00000001421AB231
  00000001421AB21E: mov         rcx,qword ptr [rbp+268h]
  00000001421AB225: lea         rdx,[anon.84ab5937f659760f8f47d75421f37c6b.1.llvm.9510372700382549680]
  00000001421AB22C: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h038804b45b640021E
  00000001421AB231: mov         rdx,qword ptr [rbp+290h]
  00000001421AB238: mov         rax,qword ptr [rdx+28h]
  00000001421AB23C: mov         rcx,r14
  00000001421AB23F: shl         rcx,4
  00000001421AB243: mov         qword ptr [rax+rcx],rbx
  00000001421AB247: mov         qword ptr [rax+rcx+8],r15
  00000001421AB24C: inc         r14
  00000001421AB24F: mov         qword ptr [rdx+30h],r14
  00000001421AB253: inc         qword ptr [rdx+40h]

; ==================================================================
; system_roof::edit_roof —— 写 ridge_length_01 前后比 is_gable，翻转时发事件与音效
  00000001421ABD97: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  00000001421ABD9C: mov         r12d,eax
  00000001421ABD9F: mov         eax,dword ptr [rbx+10h]
  00000001421ABDA2: test        eax,eax
  00000001421ABDA4: je          00000001421ABDF0
  00000001421ABDA6: cmp         eax,2
  00000001421ABDA9: jne         00000001421ABE00
  00000001421ABDAB: movss       xmm9,dword ptr [rbx+14h]
  00000001421ABDB1: mov         eax,dword ptr [rsp+28h]
  00000001421ABDB5: mov         dword ptr [rsi+rdi*4],eax
  00000001421ABDB8: movss       xmm0,dword ptr [r15+34h]
  00000001421ABDBE: addss       xmm0,xmm9
  00000001421ABDC3: xorps       xmm1,xmm1
  00000001421ABDC6: maxss       xmm1,xmm0
  00000001421ABDCA: movaps      xmm0,xmm14
  00000001421ABDCE: minss       xmm0,xmm1
  00000001421ABDD2: movss       dword ptr [r15+34h],xmm0
  00000001421ABDD8: mov         rcx,r15
  00000001421ABDDB: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  00000001421ABDE0: mov         edi,eax
  00000001421ABDE2: jmp         00000001421ABE48
  00000001421ABDE4: nop         word ptr cs:[rax+rax]
  00000001421ABDF0: xorps       xmm9,xmm9
  00000001421ABDF4: mov         edi,r12d
  00000001421ABDF7: jmp         00000001421ABE48
  00000001421ABDF9: nop         dword ptr [rax]
  00000001421ABE00: movaps      xmm11,xmm12
  00000001421ABE04: movaps      xmm12,xmm10
  00000001421ABE08: movss       xmm9,dword ptr [rbx+14h]
  00000001421ABE0E: xorps       xmm0,xmm0
  00000001421ABE11: maxss       xmm0,dword ptr [r15+34h]
  00000001421ABE17: movaps      xmm10,xmm14
  00000001421ABE1B: minss       xmm10,xmm0
  00000001421ABE20: mov         eax,dword ptr [rsp+28h]
  00000001421ABE24: mov         dword ptr [rsi+rdi*4],eax
  00000001421ABE27: movss       dword ptr [r15+34h],xmm9
  00000001421ABE2D: mov         rcx,r15
  00000001421ABE30: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  00000001421ABE35: mov         edi,eax
  00000001421ABE37: subss       xmm9,xmm10
  00000001421ABE3C: movaps      xmm10,xmm12
  00000001421ABE40: movaps      xmm12,xmm11
  00000001421ABE44: xorps       xmm11,xmm11
  00000001421ABE48: ucomiss     xmm7,xmm11
  00000001421ABE4C: setne       cl
  00000001421ABE4F: ucomiss     xmm8,xmm11
  00000001421ABE53: setne       al
  00000001421ABE56: cmp         r12b,dil
  00000001421ABE59: jne         00000001421ABEB0
  00000001421ABE5B: or          cl,bpl
  00000001421ABE5E: and         cl,dil
  00000001421ABE61: jne         00000001421ABEB0
  00000001421ABE63: and         al,dil
  00000001421ABE66: jne         00000001421ABEB0
  00000001421ABE68: test        r12b,r12b
  00000001421ABE6B: je          00000001421ABF1E
  00000001421ABE71: test        dil,dil
  00000001421ABE74: mov         r12,r13
  00000001421ABE77: mov         r13,qword ptr [rsp+0B8h]
  00000001421ABE7F: mov         rbp,qword ptr [rsp+88h]
  00000001421ABE87: jne         00000001421ABFA7
  00000001421ABE8D: lea         rcx,[rsp+50h]
  00000001421ABE92: mov         rdx,r15
  00000001421ABE95: call        _ZN12country_core9resources5roofs10roof_shape4Roof8pivot_3d17h4bc65dfe321d2fa5E
  00000001421ABE9A: mov         byte ptr [rsp+40h],0Bh
  00000001421ABE9F: jmp         00000001421ABF48
  00000001421ABEA4: nop         word ptr cs:[rax+rax]
  00000001421ABEB0: mov         rbp,qword ptr [r15]
  00000001421ABEB3: mov         eax,dword ptr [rsp+94h]
  00000001421ABEBA: mov         rcx,qword ptr [rsp+0F0h]
  00000001421ABEC2: mov         dword ptr [rcx],eax
  00000001421ABEC4: mov         rax,qword ptr [rsp+0B0h]
  00000001421ABECC: mov         rsi,qword ptr [rax+30h]
  00000001421ABED0: mov         r14,qword ptr [rax+40h]
  00000001421ABED4: cmp         rsi,qword ptr [rax+20h]
  00000001421ABED8: jne         00000001421ABEEE
  00000001421ABEDA: mov         rcx,qword ptr [rsp+0D8h]
  00000001421ABEE2: lea         rdx,[anon.84ab5937f659760f8f47d75421f37c6b.1.llvm.9510372700382549680]
  00000001421ABEE9: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h038804b45b640021E
  00000001421ABEEE: mov         rdx,qword ptr [rsp+0B0h]
  00000001421ABEF6: mov         rax,qword ptr [rdx+28h]
  00000001421ABEFA: mov         rcx,rsi
  00000001421ABEFD: shl         rcx,4
  00000001421ABF01: mov         qword ptr [rax+rcx],r14
  00000001421ABF05: mov         qword ptr [rax+rcx+8],rbp
  00000001421ABF0A: inc         rsi
  00000001421ABF0D: mov         qword ptr [rdx+30h],rsi
  00000001421ABF11: inc         qword ptr [rdx+40h]
  00000001421ABF15: test        r12b,r12b
  00000001421ABF18: jne         00000001421ABE71
  00000001421ABF1E: test        dil,dil
  00000001421ABF21: mov         r12,r13
  00000001421ABF24: mov         r13,qword ptr [rsp+0B8h]
  00000001421ABF2C: mov         rbp,qword ptr [rsp+88h]
  00000001421ABF34: je          00000001421ABFA7
  00000001421ABF36: lea         rcx,[rsp+50h]
  00000001421ABF3B: mov         rdx,r15
  00000001421ABF3E: call        _ZN12country_core9resources5roofs10roof_shape4Roof8pivot_3d17h4bc65dfe321d2fa5E
  00000001421ABF43: mov         byte ptr [rsp+40h],0Ah

; ==================================================================
; ui_edit_roof_ridge_length::create_ui_widget —— 两个手柄：端点外 0.75 m 处的 1 × 1 × 1.5 m 触发盒
  00000001421AA1EC: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims4from17hcb00f4a21c308a29E
  00000001421AA1F1: mov         qword ptr [rbp+180h],rax
  00000001421AA1F8: lea         rcx,[rbp+0C4h]
  00000001421AA1FF: lea         rdx,[rbp+180h]
  00000001421AA206: mov         r8,r14
  00000001421AA209: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims14to_world_space17h36c23ee40b8867c8E
  00000001421AA20E: mov         rcx,r14
  00000001421AA211: call        _ZN12country_core9resources5roofs10roof_shape4Roof2ty17h001241e6e27ea1b3E
  00000001421AA216: test        al,al
  00000001421AA218: je          00000001421AA8B1
  00000001421AA21E: cmp         dword ptr [r14+8],1
  00000001421AA223: jne         00000001421AA8B1
  00000001421AA229: mov         rax,qword ptr [r14+1Ch]
  00000001421AA22D: mov         qword ptr [rbp+280h],rax
  00000001421AA234: movups      xmm0,xmmword ptr [r14+0Ch]
  00000001421AA239: movaps      xmmword ptr [rbp+270h],xmm0
  00000001421AA240: lea         rax,[rbp+0C8h]
  00000001421AA247: test        byte ptr [rbp+0C4h],1
  00000001421AA24E: je          00000001421AA2EE
  00000001421AA254: mov         rcx,qword ptr [rax+10h]
  00000001421AA258: mov         qword ptr [rbp+2D0h],rcx
  00000001421AA25F: movups      xmm0,xmmword ptr [rax]
  00000001421AA262: movaps      xmmword ptr [rbp+2C0h],xmm0
  00000001421AA269: cmp         byte ptr [r14+3Ch],0
  00000001421AA26E: je          00000001421AA31F
  00000001421AA274: movss       xmm8,dword ptr [rbp+270h]
  00000001421AA27D: movss       xmm9,dword ptr [rbp+274h]
  00000001421AA286: movaps      xmm10,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421AA28E: xorps       xmm9,xmm10
  00000001421AA292: movss       dword ptr [rbp+10h],xmm9
  00000001421AA298: movss       dword ptr [rbp+14h],xmm8
  00000001421AA29E: lea         rcx,[rbp+180h]
  00000001421AA2A5: lea         rdx,[rbp+10h]
  00000001421AA2A9: xorps       xmm2,xmm2
  00000001421AA2AC: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421AA2B1: movsd       xmm6,mmword ptr [rbp+180h]
  00000001421AA2B9: xorps       xmm6,xmm10
  00000001421AA2BD: movss       xmm7,dword ptr [rbp+188h]
  00000001421AA2C5: xorps       xmm7,xmm10
  00000001421AA2C9: movss       dword ptr [rbp+180h],xmm9
  00000001421AA2D2: movss       dword ptr [rbp+184h],xmm8
  00000001421AA2DB: lea         rcx,[rbp+2ACh]
  00000001421AA2E2: lea         rdx,[rbp+180h]
  00000001421AA2E9: xorps       xmm2,xmm2
  00000001421AA2EC: jmp         00000001421AA362
  00000001421AA2EE: mov         ecx,dword ptr [rax+8]
  00000001421AA2F1: mov         dword ptr [rbp+2D4h],ecx
  00000001421AA2F7: mov         rcx,qword ptr [rax]
  00000001421AA2FA: mov         qword ptr [rbp+2CCh],rcx
  00000001421AA301: mov         rcx,qword ptr [rax]
  00000001421AA304: mov         qword ptr [rbp+2C0h],rcx
  00000001421AA30B: mov         eax,dword ptr [rax+8]
  00000001421AA30E: mov         dword ptr [rbp+2C8h],eax
  00000001421AA314: cmp         byte ptr [r14+3Ch],0
  00000001421AA319: jne         00000001421AA274
  00000001421AA31F: lea         rcx,[rbp+180h]
  00000001421AA326: lea         rsi,[rbp+270h]
  00000001421AA32D: xorps       xmm2,xmm2
  00000001421AA330: mov         rdx,rsi
  00000001421AA333: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421AA338: movsd       xmm6,mmword ptr [rbp+180h]
  00000001421AA340: movaps      xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  00000001421AA347: xorps       xmm6,xmm0
  00000001421AA34A: movss       xmm7,dword ptr [rbp+188h]
  00000001421AA352: xorps       xmm7,xmm0
  00000001421AA355: lea         rcx,[rbp+2ACh]
  00000001421AA35C: xorps       xmm2,xmm2
  00000001421AA35F: mov         rdx,rsi
  00000001421AA362: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  00000001421AA367: mov         qword ptr [rbp+2B8h],r14
  00000001421AA36E: mov         qword ptr [rbp+2A0h],rbx
  00000001421AA375: mov         qword ptr [rbp+298h],rdi
  00000001421AA37C: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001421AA383: mov         ecx,30h
  00000001421AA388: mov         edx,8
  00000001421AA38D: call        __rust_alloc
  00000001421AA392: test        rax,rax
  00000001421AA395: je          00000001421AA936
  00000001421AA39B: movzx       ecx,byte ptr [rbp+418h]
  00000001421AA3A2: mov         qword ptr [rbp+2E8h],2
  00000001421AA3AD: mov         qword ptr [rbp+2F0h],rax
  00000001421AA3B4: mov         qword ptr [rbp+2F8h],0
  00000001421AA3BF: mov         rax,qword ptr [rbp+2D0h]
  00000001421AA3C6: mov         qword ptr [rbp+130h],rax
  00000001421AA3CD: movaps      xmm0,xmmword ptr [rbp+2C0h]
  00000001421AA3D4: movups      xmmword ptr [rbp+120h],xmm0
  00000001421AA3DB: mov         rax,qword ptr [rbp+2ACh]
  00000001421AA3E2: mov         qword ptr [rbp+154h],rax
  00000001421AA3E9: mov         eax,dword ptr [rbp+2B4h]
  00000001421AA3EF: mov         dword ptr [rbp+15Ch],eax
  00000001421AA3F5: xorps       xmm0,xmm0
  00000001421AA3F8: movups      xmmword ptr [rbp+160h],xmm0
  00000001421AA3FF: mov         qword ptr [rbp+170h],0
  00000001421AA40A: mov         qword ptr [rbp+110h],0
  00000001421AA415: mov         qword ptr [rbp+118h],2
  00000001421AA420: mov         qword ptr [rbp+140h],2
  00000001421AA42B: movlps      qword ptr [rbp+148h],xmm6
  00000001421AA432: movss       dword ptr [rbp+150h],xmm7
  00000001421AA43A: xor         cl,1
  00000001421AA43D: mov         byte ptr [rbp+304h],cl
  00000001421AA443: xor         esi,esi
  00000001421AA445: lea         r13,[rbp+10h]
  00000001421AA449: lea         rdx,[rbp-50h]
  00000001421AA44D: movss       xmm8,dword ptr [__real@3f400000]
  00000001421AA456: movaps      xmm9,xmmword ptr [__xmm@00000000000000003f4000003f400000]
  00000001421AA45E: movss       xmm6,dword ptr [__real@3f800000]
  00000001421AA466: movss       xmm7,dword ptr [__real@3fc00000]
  00000001421AA46E: movss       xmm10,dword ptr [__real@3f99999a]
  00000001421AA477: xor         eax,eax
  00000001421AA479: xor         ecx,ecx
  00000001421AA47B: xor         r12d,r12d
  00000001421AA47E: jmp         00000001421AA4B9
  00000001421AA480: inc         r12
  00000001421AA483: mov         rax,qword ptr [rbp+2F0h]
  00000001421AA48A: lea         rcx,[rsi+rsi*2]
  00000001421AA48E: mov         qword ptr [rax+rcx*8],rdi
  00000001421AA492: movss       dword ptr [rax+rcx*8+8],xmm13
  00000001421AA499: movlps      qword ptr [rax+rcx*8+0Ch],xmm11
  00000001421AA49F: movss       dword ptr [rax+rcx*8+14h],xmm12
  00000001421AA4A6: inc         rsi
  00000001421AA4A9: mov         qword ptr [rbp+2F8h],rsi
  00000001421AA4B0: mov         rax,r14
  00000001421AA4B3: mov         rcx,rbx
  00000001421AA4B6: mov         rdx,r15
  00000001421AA4B9: cmp         r12,2
  00000001421AA4BD: je          00000001421AA78E
  00000001421AA4C3: cmp         rcx,2
  00000001421AA4C7: je          00000001421AA78E
  00000001421AA4CD: lea         r9,[r12+r12*2]
  00000001421AA4D1: lea         r8,[rbp+120h]
  00000001421AA4D8: movsd       xmm14,mmword ptr [r8+r9*4]
  00000001421AA4DE: movss       xmm13,dword ptr [r8+r9*4+8]
  00000001421AA4E5: lea         rbx,[rcx+1]
  00000001421AA4E9: mov         qword ptr [rbp+138h],rbx
  00000001421AA4F0: lea         rcx,[rcx+rcx*2]
  00000001421AA4F4: movsd       xmm11,mmword ptr [r8+rcx*4+28h]
  00000001421AA4FB: movss       xmm12,dword ptr [r8+rcx*4+30h]
  00000001421AA502: lea         r14,[rax+1]
  00000001421AA506: mov         qword ptr [rbp+178h],r14
  00000001421AA50D: mov         qword ptr [rbp+290h],rax
  00000001421AA514: movsd       mmword ptr [rbp-50h],xmm11
  00000001421AA51A: movss       dword ptr [rbp-48h],xmm12
  00000001421AA520: mov         rcx,r13
  00000001421AA523: mov         r15,rdx
  00000001421AA526: call        _ZN5utils20rotation_from_normal17hd429cabdc32e2d67E
  00000001421AA52B: lea         rsi,[rbp+180h]
  00000001421AA532: movaps      xmm0,xmm12
  00000001421AA536: mulss       xmm0,xmm8
  00000001421AA53B: addss       xmm13,xmm0
  00000001421AA540: movaps      xmm0,xmmword ptr [rbp+10h]
  00000001421AA544: movaps      xmm1,xmm11
  00000001421AA548: mulps       xmm1,xmm9
  00000001421AA54C: addps       xmm14,xmm1
  00000001421AA550: movlps      qword ptr [rbp+190h],xmm14
  00000001421AA558: movss       dword ptr [rbp+198h],xmm13
  00000001421AA561: movaps      xmmword ptr [rbp+180h],xmm0
  00000001421AA568: lea         rax,[rbp+19Ch]
  00000001421AA56F: mov         rcx,3F8000003F800000h
  00000001421AA579: mov         qword ptr [rax],rcx
  00000001421AA57C: mov         dword ptr [rax+8],3F800000h
  00000001421AA583: mov         qword ptr [rsp+20h],rsi
  00000001421AA588: lea         rcx,[rbp+238h]
  00000001421AA58F: movaps      xmm1,xmm6
  00000001421AA592: movaps      xmm2,xmm6
  00000001421AA595: movaps      xmm3,xmm7
  00000001421AA598: call        _ZN12country_core7systems4wall6shapes9rectangle29ui_edit_rectangle_orientation18box_trigger_volume17ha941de60e296844fE

; ==================================================================
; ui_edit_roof_ridge_length::do_ui_edit_roof_ridge_length —— 起拖记 offset；拖拽中 len = 2·|P − C| − offset → inverse_ridge_01_param → EditRoofCmd
  00000001421A959F: mov         rcx,rdi
  00000001421A95A2: mov         rdx,qword ptr [rbp+88h]
  00000001421A95A9: call        _ZN12country_core5utils23project_cursor_onto_ray23ProportionalScreenSpace16from_window_size17h6df22505b9147620E
  00000001421A95AE: mov         rax,qword ptr [rbp+90h]
  00000001421A95B5: movss       xmm1,dword ptr [rax]
  00000001421A95B9: movss       xmm2,dword ptr [rax+4]
  00000001421A95BE: mov         rcx,rdi
  00000001421A95C1: call        _ZN12country_core5utils23project_cursor_onto_ray23ProportionalScreenSpace22transform_screen_space17h27bc47c8355f5147E
  00000001421A95C6: movaps      xmm2,xmm1
  00000001421A95C9: movss       dword ptr [rbp+0F0h],xmm9
  00000001421A95D2: movss       dword ptr [rbp+0F4h],xmm7
  00000001421A95DA: movss       dword ptr [rbp+0F8h],xmm8
  00000001421A95E3: mov         eax,dword ptr [rbp+148h]
  00000001421A95E9: lea         rcx,[rbp+0FCh]
  00000001421A95F0: mov         dword ptr [rcx+8],eax
  00000001421A95F3: mov         rax,qword ptr [rbp+140h]
  00000001421A95FA: mov         qword ptr [rcx],rax
  00000001421A95FD: movsd       mmword ptr [rbp+0C0h],xmm11
  00000001421A9606: mov         byte ptr [rbp+0C8h],0
  00000001421A960D: lea         rax,[rbp+0C0h]
  00000001421A9614: mov         qword ptr [rsp+30h],rax
  00000001421A9619: mov         qword ptr [rsp+28h],r12
  00000001421A961E: mov         rax,qword ptr [rbp+98h]
  00000001421A9625: mov         qword ptr [rsp+20h],rax
  00000001421A962A: lea         rcx,[rbp+134h]
  00000001421A9631: movaps      xmm1,xmm0
  00000001421A9634: mov         r9,rdi
  00000001421A9637: call        _ZN12country_core5utils23project_cursor_onto_ray28ProportionalScreenSpaceCoord16project_onto_ray17h03ec538b60b3e45bE
  00000001421A963C: movss       xmm0,dword ptr [rbp+138h]
  00000001421A9644: subss       xmm0,xmm7
  00000001421A9648: mulss       xmm0,xmm0
  00000001421A964C: movss       xmm1,dword ptr [rbp+134h]
  00000001421A9654: movss       xmm2,dword ptr [rbp+13Ch]
  00000001421A965C: unpcklps    xmm2,xmm1
  00000001421A965F: subps       xmm2,xmm8
  00000001421A9663: mulps       xmm2,xmm2
  00000001421A9666: movshdup    xmm1,xmm2
  00000001421A966A: addss       xmm1,xmm0
  00000001421A966E: addss       xmm1,xmm2
  00000001421A9672: xorps       xmm0,xmm0
  00000001421A9675: sqrtss      xmm0,xmm1
  00000001421A9679: addss       xmm0,xmm0
  00000001421A967D: subss       xmm0,xmm10
  00000001421A9682: mov         rax,qword ptr [rbp+2A0h]
  00000001421A9689: movss       dword ptr [rax],xmm0
  00000001421A968D: mov         eax,dword ptr [rbp+0BCh]
  00000001421A9693: mov         rcx,qword ptr [rbp+50h]
  00000001421A9697: mov         dword ptr [rcx],eax
  00000001421A9699: mov         rax,qword ptr [rbp+160h]
  00000001421A96A0: mov         rdx,qword ptr [rax]
  00000001421A96A3: mov         al,1
  00000001421A96A5: mov         qword ptr [rbp+0A8h],rax
  00000001421A96AC: mov         rcx,qword ptr [rbp+48h]
  00000001421A96B0: mov         r8,qword ptr [rbp+58h]
  00000001421A96B4: call        _ZN12country_core9resources5walls19decorator_archivist18DecoratorArchivist25decorator_cull_allow_wall17h05c0a47913379af5E
  00000001421A96B9: jmp         00000001421A93A4
  00000001421A96BE: mov         rcx,rdi
  00000001421A96C1: mov         rdx,qword ptr [rbp+88h]
  00000001421A96C8: call        _ZN12country_core5utils23project_cursor_onto_ray23ProportionalScreenSpace16from_window_size17h6df22505b9147620E
  00000001421A96CD: mov         rax,qword ptr [rbp+90h]
  00000001421A96D4: movss       xmm1,dword ptr [rax]
  00000001421A96D8: movss       xmm2,dword ptr [rax+4]
  00000001421A96DD: mov         rcx,rdi
  00000001421A96E0: call        _ZN12country_core5utils23project_cursor_onto_ray23ProportionalScreenSpace22transform_screen_space17h27bc47c8355f5147E
  00000001421A96E5: movaps      xmm2,xmm1
  00000001421A96E8: movss       dword ptr [rbp+0F0h],xmm9
  00000001421A96F1: movss       dword ptr [rbp+0F4h],xmm7
  00000001421A96F9: movss       dword ptr [rbp+0F8h],xmm8
  00000001421A9702: mov         eax,dword ptr [rbp+148h]
  00000001421A9708: lea         rcx,[rbp+0FCh]
  00000001421A970F: mov         dword ptr [rcx+8],eax
  00000001421A9712: mov         rax,qword ptr [rbp+140h]
  00000001421A9719: mov         qword ptr [rcx],rax
  00000001421A971C: movsd       mmword ptr [rbp+0C0h],xmm11
  00000001421A9725: mov         byte ptr [rbp+0C8h],0
  00000001421A972C: lea         rax,[rbp+0C0h]
  00000001421A9733: mov         qword ptr [rsp+30h],rax
  00000001421A9738: mov         qword ptr [rsp+28h],r12
  00000001421A973D: mov         rax,qword ptr [rbp+98h]
  00000001421A9744: mov         qword ptr [rsp+20h],rax
  00000001421A9749: lea         rcx,[rbp+134h]
  00000001421A9750: movaps      xmm1,xmm0
  00000001421A9753: mov         r9,rdi
  00000001421A9756: call        _ZN12country_core5utils23project_cursor_onto_ray28ProportionalScreenSpaceCoord16project_onto_ray17h03ec538b60b3e45bE
  00000001421A975B: movss       xmm14,dword ptr [rbp+134h]
  00000001421A9764: movss       xmm13,dword ptr [rbp+138h]
  00000001421A976D: movss       xmm12,dword ptr [rbp+13Ch]
  00000001421A9776: mov         rax,qword ptr [rbp+2A0h]
  00000001421A977D: movss       xmm6,dword ptr [rax]
  00000001421A9781: mov         rcx,qword ptr [rbp+160h]
  00000001421A9788: mov         r15b,byte ptr [rcx+3Ch]
  00000001421A978C: call        _ZN12country_core9resources5roofs10roof_shape4Roof10shape_dims17hb2956537a15373b8E
  00000001421A9791: movaps      xmm2,xmm0
  00000001421A9794: unpcklps    xmm12,xmm14
  00000001421A9798: subps       xmm12,xmm8
  00000001421A979C: subss       xmm13,xmm7
  00000001421A97A1: mulss       xmm13,xmm13
  00000001421A97A6: mulps       xmm12,xmm12
  00000001421A97AA: movshdup    xmm0,xmm12
  00000001421A97AF: addss       xmm0,xmm13
  00000001421A97B4: addss       xmm0,xmm12
  00000001421A97B9: sqrtss      xmm0,xmm0
  00000001421A97BD: addss       xmm0,xmm0
  00000001421A97C1: subss       xmm0,xmm6
  00000001421A97C5: mov         edx,r15d
  00000001421A97C8: movaps      xmm3,xmm1
  00000001421A97CB: call        _ZN12country_core9resources5roofs10roof_ridge22inverse_ridge_01_param17hac279c2a78f86561E
  00000001421A97D0: movaps      xmm6,xmm0
  00000001421A97D3: mov         eax,1
  00000001421A97D8: mov         qword ptr [rbp+0B0h],rax
  00000001421A97DF: jmp         00000001421A93A4
  00000001421A97E4: cmp         dword ptr [rbp+0B0h],0
  00000001421A97EB: je          00000001421A9859
  00000001421A97ED: mov         rax,qword ptr [rbp+160h]
  00000001421A97F4: mov         rax,qword ptr [rax]
  00000001421A97F7: mov         qword ptr [rbp+110h],rax
  00000001421A97FE: mov         byte ptr [rbp+124h],0
  00000001421A9805: mov         dword ptr [rbp+0F0h],0
  00000001421A980F: mov         dword ptr [rbp+0F8h],0
  00000001421A9819: mov         dword ptr [rbp+118h],0
  00000001421A9823: mov         dword ptr [rbp+100h],1
  00000001421A982D: movss       dword ptr [rbp+104h],xmm6
  00000001421A9835: mov         dword ptr [rbp+108h],0
  00000001421A983F: lea         r8,[142FF71F8h]
  00000001421A9846: lea         rdx,[rbp+0F0h]
  00000001421A984D: mov         rcx,qword ptr [rbp+288h]
  00000001421A9854: call        _ZN8bevy_ecs5event6writer20EventWriter$LT$E$GT$5write17h6a01ed2eb9fa1874E

; ==================================================================
; do_ui_edit_roof_ridge_length 的 signifier 闭包 —— brush_arrow_flat_single_foward（字符串在 142FF72C0，30 字节），scale × 2，端点外 0.2 m
; system_roof::ui_edit_roof_ridge_length::do_ui_edit_roof_ridge_length::closure$1:
  00000001421A9EF0: push        rbp
  00000001421A9EF1: push        r14
  00000001421A9EF3: push        rsi
  00000001421A9EF4: push        rdi
  00000001421A9EF5: push        rbx
  00000001421A9EF6: sub         rsp,180h
  00000001421A9EFD: lea         rbp,[rsp+80h]
  00000001421A9F05: mov         qword ptr [rbp+0F8h],0FFFFFFFFFFFFFFFEh
  00000001421A9F10: mov         rdi,r9
  00000001421A9F13: mov         rbx,r8
  00000001421A9F16: mov         r14,rdx
  00000001421A9F19: mov         rsi,rcx
  00000001421A9F1C: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001421A9F23: mov         ecx,8
  00000001421A9F28: mov         edx,8
  00000001421A9F2D: call        __rust_alloc
  00000001421A9F32: test        rax,rax
  00000001421A9F35: je          00000001421AA0BE
  00000001421A9F3B: mov         qword ptr [rbp+0E0h],rax
  00000001421A9F42: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001421A9F49: mov         ecx,1Eh
  00000001421A9F4E: mov         edx,1
  00000001421A9F53: call        __rust_alloc
  00000001421A9F58: test        rax,rax
  00000001421A9F5B: je          00000001421AA0CD
  00000001421A9F61: movups      xmm0,xmmword ptr [142FF72CEh]
  00000001421A9F68: movups      xmmword ptr [rax+0Eh],xmm0
  00000001421A9F6C: movups      xmm0,xmmword ptr [142FF72C0h]
  00000001421A9F73: movups      xmmword ptr [rax],xmm0
  00000001421A9F76: mov         qword ptr [rbp+48h],1Eh
  00000001421A9F7E: mov         qword ptr [rbp+50h],rax
  00000001421A9F82: mov         qword ptr [rbp+58h],1Eh
  00000001421A9F8A: mov         rax,8000000000000000h
  00000001421A9F94: mov         qword ptr [rbp+40h],rax
  00000001421A9F98: lea         rcx,[rbp-50h]
  00000001421A9F9C: lea         rdx,[rbp+40h]
  00000001421A9FA0: call        _ZN12country_core7systems2ui10signifiers22SignifierEntityBuilder3new17hcaaf6aeae9f8a58fE
  00000001421A9FA5: movss       xmm2,dword ptr [r14]
  00000001421A9FAA: addss       xmm2,xmm2
  00000001421A9FAE: lea         rcx,[rbp+40h]
  00000001421A9FB2: lea         rdx,[rbp-50h]
  00000001421A9FB6: call        _ZN12country_core7systems2ui10signifiers22SignifierEntityBuilder5scale17h6a4886f859f7dddcE
  00000001421A9FBB: lea         rcx,[rbp-50h]
  00000001421A9FBF: lea         rdx,[rbp+40h]
  00000001421A9FC3: xorps       xmm2,xmm2
  00000001421A9FC6: call        _ZN12country_core7systems2ui10signifiers22SignifierEntityBuilder18animation_duration17h2a0f1a4e546b59d9E
  00000001421A9FCB: movss       xmm0,dword ptr [r14+18h]
  00000001421A9FD1: mulss       xmm0,dword ptr [__real@3e4ccccd]
  00000001421A9FD9: addss       xmm0,dword ptr [r14+0Ch]
  00000001421A9FDF: movsd       xmm1,mmword ptr [r14+10h]
  00000001421A9FE5: mulps       xmm1,xmmword ptr [__xmm@00000000000000003e4ccccd3e4ccccd]
  00000001421A9FEC: movsd       xmm2,mmword ptr [r14+4]
  00000001421A9FF2: addps       xmm1,xmm2
  00000001421A9FF5: movlps      qword ptr [rbp+0E8h],xmm1
  00000001421A9FFC: movss       dword ptr [rbp+0F0h],xmm0
  00000001421AA004: lea         rcx,[rbp+40h]
  00000001421AA008: lea         rdx,[rbp-50h]
  00000001421AA00C: lea         r8,[rbp+0E8h]
  00000001421AA013: call        _ZN12country_core7systems2ui10signifiers22SignifierEntityBuilder8position17habd7ef803619d703E
  00000001421AA018: add         r14,10h
  00000001421AA01C: mov         eax,dword ptr [r14+8]
  00000001421AA020: mov         dword ptr [rbp+0F0h],eax
  00000001421AA026: mov         rax,qword ptr [r14]
  00000001421AA029: mov         qword ptr [rbp+0E8h],rax
  00000001421AA030: mov         byte ptr [rbp+0F7h],1
  00000001421AA037: lea         rcx,[rbp-50h]
  00000001421AA03B: lea         rdx,[rbp+0E8h]
  00000001421AA042: call        _ZN5utils20rotation_from_normal17hd429cabdc32e2d67E
  00000001421AA047: movaps      xmm0,xmmword ptr [rbp-50h]
  00000001421AA04B: movaps      xmmword ptr [rbp+0D0h],xmm0
  00000001421AA052: mov         byte ptr [rbp+0F7h],0
  00000001421AA059: lea         rcx,[rbp-50h]
  00000001421AA05D: lea         rdx,[rbp+40h]
  00000001421AA061: lea         r8,[rbp+0D0h]
  00000001421AA068: call        _ZN12country_core7systems2ui10signifiers22SignifierEntityBuilder8rotation17he3dec6d9ce82f003E
  00000001421AA06D: mov         rax,qword ptr [rbp+150h]
  00000001421AA074: mov         qword ptr [rsp+20h],rax
  00000001421AA079: lea         rcx,[rbp+40h]
  00000001421AA07D: lea         rdx,[rbp-50h]
  00000001421AA081: mov         r8,rbx
  00000001421AA084: mov         r9,rdi
  00000001421AA087: call        _ZN12country_core7systems2ui10signifiers22SignifierEntityBuilder5build17hf564dd3c41175a8bE
  00000001421AA08C: mov         rax,qword ptr [rbp+60h]

; ==================================================================
; system_wall_constructor::init_wall_height_and_arch_segment_height —— 山墙边逐点抬墙顶：c + H·(1 − s / W)^k；max_y_gable
  000000014078BC9A: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState12is_rectangle17h4cad4754e8e6b575E
  000000014078BC9F: test        byte ptr [rbp-4Ch],1
  000000014078BCA3: je          000000014078BEEE
  000000014078BCA9: lea         rcx,[rbp-48h]
  000000014078BCAD: mov         rax,qword ptr [rcx+10h]
  000000014078BCB1: mov         qword ptr [rbp-20h],rax
  000000014078BCB5: movups      xmm0,xmmword ptr [rcx]
  000000014078BCB8: movaps      xmmword ptr [rbp-30h],xmm0
  000000014078BCBC: mov         rcx,r13
  000000014078BCBF: mov         rdx,r14
  000000014078BCC2: call        _ZN12country_core9resources5walls12public_walls11PublicWalls11roof_entity17h8ce15f3ac3006d60E
  000000014078BCC7: mov         rcx,rax
  000000014078BCCA: shr         rcx,20h
  000000014078BCCE: mov         r12,qword ptr [rbp+0A8h]
  000000014078BCD5: mov         rbx,qword ptr [rbp+0D0h]
  000000014078BCDC: mov         r11,qword ptr [rbp+70h]
  000000014078BCE0: je          000000014078C42D
  000000014078BCE6: mov         r8d,eax
  000000014078BCE9: mov         rdx,qword ptr [rbp+88h]
  000000014078BCF0: cmp         r8,qword ptr [rdx+10h]
  000000014078BCF4: jae         000000014078C667
  000000014078BCFA: mov         rdx,qword ptr [rdx+8]
  000000014078BCFE: mov         rcx,rax
  000000014078BD01: shr         rcx,20h
  000000014078BD05: lea         r9,[r8+r8*4]
  000000014078BD09: cmp         dword ptr [rdx+r9*4],ecx
  000000014078BD0D: jne         000000014078C667
  000000014078BD13: lea         r9,[rdx+r9*4]
  000000014078BD17: mov         edx,dword ptr [r9+4]
  000000014078BD1B: mov         r10d,0FFFFFFFFh
  000000014078BD21: cmp         rdx,r10
  000000014078BD24: je          000000014078C667
  000000014078BD2A: xor         r8d,r8d
  000000014078BD2D: cmp         qword ptr [r11+38h],rdx
  000000014078BD31: jbe         000000014078C677
  000000014078BD37: mov         rcx,qword ptr [r11+28h]
  000000014078BD3B: mov         r10d,edx
  000000014078BD3E: shr         r10d,6
  000000014078BD42: mov         rcx,qword ptr [rcx+r10*8]
  000000014078BD46: bt          rcx,rdx
  000000014078BD4A: jae         000000014078C677
  000000014078BD50: mov         eax,dword ptr [r9+0Ch]
  000000014078BD54: mov         ecx,dword ptr [r9+10h]
  000000014078BD58: mov         rdx,qword ptr [rbp+88h]
  000000014078BD5F: mov         rdx,qword ptr [rdx+1A8h]
  000000014078BD66: lea         rax,[rax+rax*8]
  000000014078BD6A: mov         r8,qword ptr [r11+110h]
  000000014078BD71: mov         r9,qword ptr [rdx+rax*8+18h]
  000000014078BD76: mov         rax,qword ptr [rdx+rax*8+38h]
  000000014078BD7B: mov         rax,qword ptr [rax+r8*8]
  000000014078BD7F: not         rax
  000000014078BD82: lea         rax,[rax+rax*2]
  000000014078BD86: shl         rax,4
  000000014078BD8A: imul        rsi,rcx,58h
  000000014078BD8E: add         rsi,qword ptr [r9+rax+10h]
  000000014078BD93: mov         rcx,rsi
  000000014078BD96: call        _ZN12country_core9resources5roofs10roof_shape4Roof8is_gable17h190fa1901a4bdb66E
  000000014078BD9B: test        al,al
  000000014078BD9D: je          000000014078C42D
  000000014078BDA3: movzx       edx,byte ptr [rsi+3Ch]
  000000014078BDA7: mov         qword ptr [rbp+0D8h],rsi
  000000014078BDAE: xor         esi,esi
  000000014078BDB0: test        rdx,rdx
  000000014078BDB3: sete        sil
  000000014078BDB7: mov         r14d,2
  000000014078BDBD: sub         r14,rsi
  000000014078BDC0: mov         r15,rsi
  000000014078BDC3: xor         r15,3
  000000014078BDC7: mov         ebx,4
  000000014078BDCC: sub         rbx,rsi
  000000014078BDCF: lea         rdi,[rbp-30h]
  000000014078BDD3: mov         rcx,rdi
  000000014078BDD6: call        _ZN5utils8geometry9rectangle11Rectangle2d11get_point_u17h90471c68e2b69caeE
  000000014078BDDB: movss       dword ptr [rbp+0B4h],xmm0
  000000014078BDE3: mov         rcx,rdi
  000000014078BDE6: mov         rdx,r14
  000000014078BDE9: call        _ZN5utils8geometry9rectangle11Rectangle2d11get_point_u17h90471c68e2b69caeE
  000000014078BDEE: movaps      xmm10,xmm0
  000000014078BDF2: mov         rcx,rdi
  000000014078BDF5: mov         rdx,r15
  000000014078BDF8: call        _ZN5utils8geometry9rectangle11Rectangle2d11get_point_u17h90471c68e2b69caeE
  000000014078BDFD: movss       dword ptr [rbp+0B8h],xmm0
  000000014078BE05: mov         rcx,rdi
  000000014078BE08: mov         rdx,rbx
  000000014078BE0B: call        _ZN5utils8geometry9rectangle11Rectangle2d11get_point_u17h90471c68e2b69caeE
  000000014078BE10: movss       dword ptr [rbp+0E4h],xmm0
  000000014078BE18: movss       xmm8,dword ptr [rbp+rsi*4-20h]
  000000014078BE1F: mov         rcx,qword ptr [rbp+0D8h]
  000000014078BE26: mov         rax,qword ptr [rbp+0D0h]
  000000014078BE2D: mov         edx,dword ptr [rbp+0F4h]
  000000014078BE33: mov         dword ptr [rax],edx
  000000014078BE35: cmp         qword ptr [r13+18h],0
  000000014078BE3A: je          000000014078C647
  000000014078BE40: lea         r15,[rcx+24h]
  000000014078BE44: mov         rcx,qword ptr [rbp+80h]
  000000014078BE4B: lea         rdx,[rbp+78h]
  000000014078BE4F: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  000000014078BE54: mov         rbx,qword ptr [r13]
  000000014078BE58: mov         rcx,qword ptr [r13+8]
  000000014078BE5C: mov         rdx,rcx
  000000014078BE5F: and         rdx,rax
  000000014078BE62: shr         rax,39h
  000000014078BE66: movd        xmm0,eax
  000000014078BE6A: punpcklbw   xmm0,xmm0
  000000014078BE6E: pshuflw     xmm0,xmm0,0
  000000014078BE73: pshufd      xmm0,xmm0,0
  000000014078BE78: lea         rax,[rbx-2A8h]
  000000014078BE7F: xor         r8d,r8d
  000000014078BE82: movdqu      xmm1,xmmword ptr [rbx+rdx]
  000000014078BE87: movdqa      xmm2,xmm1
  000000014078BE8B: pcmpeqb     xmm2,xmm0
  000000014078BE8F: pmovmskb    r9d,xmm2
  000000014078BE94: test        r9d,r9d
  000000014078BE97: mov         r11,qword ptr [rbp+0C8h]
  000000014078BE9E: je          000000014078BECC
  000000014078BEA0: tzcnt       r10d,r9d
  000000014078BEA5: add         r10,rdx
  000000014078BEA8: and         r10,rcx
  000000014078BEAB: neg         r10
  000000014078BEAE: imul        rsi,r10,2A8h
  000000014078BEB5: cmp         r11,qword ptr [rax+rsi]
  000000014078BEB9: je          000000014078BF83
  000000014078BEBF: lea         r10d,[r9-1]
  000000014078BEC3: and         r10w,r9w
  000000014078BEC7: mov         r9d,r10d
  000000014078BECA: jne         000000014078BEA0
  000000014078BECC: pcmpeqb     xmm1,xmm6
  000000014078BED0: pmovmskb    r9d,xmm1
  000000014078BED5: test        r9d,r9d
  000000014078BED8: jne         000000014078C647
  000000014078BEDE: add         rdx,r8
  000000014078BEE1: add         rdx,10h
  000000014078BEE5: add         r8,10h
  000000014078BEE9: and         rdx,rcx
  000000014078BEEC: jmp         000000014078BE82
  000000014078BEEE: mov         rdx,qword ptr [rbp+0C0h]
  000000014078BEF5: mov         rax,rdi
  000000014078BEF8: mov         r12,qword ptr [rbp+0A8h]
  000000014078BEFF: mov         rbx,qword ptr [rbp+0D0h]
  000000014078BF06: jmp         000000014078B6C0
  000000014078BF0B: movss       xmm7,dword ptr [rbx-1Ch]
  000000014078BF10: addss       xmm7,xmm8
  000000014078BF15: maxss       xmm7,xmm10
  000000014078BF1A: mov         qword ptr [rbx-1B8h],0
  000000014078BF25: xor         eax,eax
  000000014078BF27: mov         r15d,dword ptr [rbp+0F4h]
  000000014078BF2E: mov         rsi,qword ptr [rbp+0E8h]
  000000014078BF35: jmp         000000014078BF53
  000000014078BF37: pxor        xmm0,xmm0
  000000014078BF3B: mov         r15d,dword ptr [rbp+0F4h]
  000000014078BF42: mov         rsi,qword ptr [rbp+0E8h]
  000000014078BF49: jmp         000000014078C55E
  000000014078BF4E: xor         eax,eax
  000000014078BF50: mov         r15d,edi
  000000014078BF53: mov         qword ptr [rbx-1B8h],rax
  000000014078BF5A: mov         dword ptr [rbx-198h],1
  000000014078BF64: movss       dword ptr [rbx-194h],xmm7
  000000014078BF6C: mov         dword ptr [rbx-190h],1
  000000014078BF76: movss       dword ptr [rbx-18Ch],xmm7
  000000014078BF7E: jmp         000000014078C582
  000000014078BF83: mov         rax,qword ptr [rbp+0D8h]
  000000014078BF8A: movss       xmm9,dword ptr [rax+2Ch]
  000000014078BF90: movaps      xmm12,xmm9
  000000014078BF94: mulss       xmm12,xmm13
  000000014078BF99: mulss       xmm8,xmm11
  000000014078BF9E: lea         rcx,[rbx+rsi]
  000000014078BFA2: add         rcx,0FFFFFFFFFFFFFFF9h
  000000014078BFA6: mov         qword ptr [rbp+0C8h],rcx
  000000014078BFAD: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  000000014078BFB2: movaps      xmm13,xmm8
  000000014078BFB6: test        al,al
  000000014078BFB8: jne         000000014078BFF2
  000000014078BFBA: mov         rcx,r15
  000000014078BFBD: call        _ZN12country_core9resources5roofs10roof_shape15RoofShapeParams14eave_length_ws17h40916087298468ffE
  000000014078BFC2: movdqa      xmm14,xmm0
  000000014078BFC7: mov         rax,qword ptr [rbp+0D8h]
  000000014078BFCE: movd        xmm2,dword ptr [rax+2Ch]
  000000014078BFD3: movaps      xmm0,xmm11
  000000014078BFD7: movd        xmm1,dword ptr [__real@3f800000]
  000000014078BFDF: call        _ZN5utils12inverse_lerp17hdcf8664c50f3232bE
  000000014078BFE4: movaps      xmm13,xmm0
  000000014078BFE8: mulss       xmm13,xmm14
  000000014078BFED: addss       xmm13,xmm8
  000000014078BFF2: add         rbx,rsi
  000000014078BFF5: addss       xmm13,dword ptr [__real@3e8f5c29]
  000000014078BFFE: movaps      xmm7,xmm11
  000000014078C002: divss       xmm7,dword ptr [rbx-14h]
  000000014078C007: mulss       xmm7,dword ptr [__real@3f733333]
  000000014078C00F: mov         rdx,qword ptr [rbx-210h]
  000000014078C016: mov         rax,qword ptr [rbx-208h]
  000000014078C01D: lea         rax,[rax+rax*2]
  000000014078C021: lea         r8,[rdx+rax*4]
  000000014078C025: lea         rsi,[rbp+20h]
  000000014078C029: mov         rcx,rsi
  000000014078C02C: lea         r9,[anon.8226dd3aab0d3f13e2fa6def72838753.1.llvm.2319672265491296867]
  000000014078C033: call        _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17h1c2cfe8d8b850492E
  000000014078C038: lea         rcx,[rbp-18h]
  000000014078C03C: mov         rdx,rsi
  000000014078C03F: xor         r8d,r8d
  000000014078C042: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$19try_new_from_points17h7570813ebb806f94E
  000000014078C047: xor         eax,eax
  000000014078C049: cmp         rax,qword ptr [rbp-18h]
  000000014078C04D: jo          000000014078C6AA
  000000014078C053: mov         rax,qword ptr [rbp+18h]
  000000014078C057: mov         qword ptr [rbp+50h],rax
  000000014078C05B: movups      xmm0,xmmword ptr [rbp-18h]
  000000014078C05F: movups      xmm1,xmmword ptr [rbp-8]
  000000014078C063: movdqu      xmm2,xmmword ptr [rbp+8]
  000000014078C068: movdqa      xmmword ptr [rbp+40h],xmm2
  000000014078C06D: movaps      xmmword ptr [rbp+30h],xmm1
  000000014078C071: movaps      xmmword ptr [rbp+20h],xmm0
  000000014078C075: mov         r15,qword ptr [rbp+40h]
  000000014078C079: mov         rcx,qword ptr [rbp+48h]
  000000014078C07D: mov         rax,qword ptr [rbx-1B8h]
  000000014078C084: cmp         rax,rcx
  000000014078C087: cmovb       rcx,rax
  000000014078C08B: test        rcx,rcx
  000000014078C08E: mov         qword ptr [rbp+90h],rbx
  000000014078C095: je          000000014078C333
  000000014078C09B: mov         rdi,rcx
  000000014078C09E: movss       xmm0,dword ptr [__real@3f800000]
  000000014078C0A6: subss       xmm0,xmm9
  000000014078C0AB: mulss       xmm0,xmm11
  000000014078C0B0: addss       xmm12,xmm0
  000000014078C0B5: divss       xmm13,xmm8
  000000014078C0BA: mov         r13,qword ptr [rbx-1C0h]
  000000014078C0C1: movaps      xmm0,xmm7
  000000014078C0C4: addss       xmm0,xmm10
  000000014078C0C9: movss       dword ptr [rbp+98h],xmm0
  000000014078C0D1: movss       xmm0,dword ptr [rbp+0B4h]
  000000014078C0D9: movaps      xmm9,xmm0
  000000014078C0DD: subss       xmm9,xmm7
  000000014078C0E2: subss       xmm10,xmm0
  000000014078C0E7: movss       dword ptr [rbp+9Ch],xmm10
  000000014078C0F0: movss       xmm0,dword ptr [rbp+0B8h]
  000000014078C0F8: movaps      xmm11,xmm0
  000000014078C0FC: subss       xmm11,xmm7
  000000014078C101: movss       xmm1,dword ptr [rbp+0E4h]
  000000014078C109: addss       xmm7,xmm1
  000000014078C10D: subss       xmm1,xmm0
  000000014078C111: movss       dword ptr [rbp+0E4h],xmm1
  000000014078C119: xor         esi,esi
  000000014078C11B: xor         ebx,ebx
  000000014078C11D: jmp         000000014078C12C
  000000014078C11F: nop
  000000014078C120: inc         rbx
  000000014078C123: cmp         rdi,rbx
  000000014078C126: je          000000014078C335
  000000014078C12C: movss       xmm0,dword ptr [r15+rbx*4]
  000000014078C132: ucomiss     xmm0,xmm9
  000000014078C136: jb          000000014078C22E
  000000014078C13C: movss       xmm1,dword ptr [rbp+98h]
  000000014078C144: ucomiss     xmm1,xmm0
  000000014078C147: jbe         000000014078C22E
  000000014078C14D: subss       xmm0,dword ptr [rbp+0B4h]
  000000014078C155: divss       xmm0,dword ptr [rbp+9Ch]
  000000014078C15D: xorps       xmm1,xmm1
  000000014078C160: maxss       xmm1,xmm0
  000000014078C164: movss       xmm8,dword ptr [__real@3f800000]
  000000014078C16D: minss       xmm8,xmm1
  000000014078C172: addss       xmm8,dword ptr [__real@bf000000]
  000000014078C17B: mov         rcx,qword ptr [rbp+0C8h]
  000000014078C182: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  000000014078C187: mov         r14b,al
  000000014078C18A: mov         rcx,qword ptr [rbp+0D8h]
  000000014078C191: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  000000014078C196: movaps      xmm14,xmm0
  000000014078C19A: pxor        xmm6,xmm6
  000000014078C19E: test        r14b,r14b
  000000014078C1A1: jne         000000014078C1AB
  000000014078C1A3: movd        xmm6,dword ptr [__real@3eeb851f]
  000000014078C1AB: andps       xmm8,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  000000014078C1B3: addss       xmm8,xmm8
  000000014078C1B8: divss       xmm8,xmm13
  000000014078C1BD: movss       xmm10,dword ptr [__real@3f800000]
  000000014078C1C6: movaps      xmm0,xmm10
  000000014078C1CA: subss       xmm0,xmm8
  000000014078C1CF: movaps      xmm1,xmm12
  000000014078C1D3: call        powf
  000000014078C1D8: addss       xmm14,xmm6
  000000014078C1DD: movaps      xmm1,xmm10
  000000014078C1E1: subss       xmm1,xmm0
  000000014078C1E5: mulss       xmm1,xmm6
  000000014078C1E9: mulss       xmm14,xmm0
  000000014078C1EE: addss       xmm14,xmm1
  000000014078C1F3: movss       xmm0,dword ptr [r13+rbx*4]
  000000014078C1FA: addss       xmm0,xmm14
  000000014078C1FF: movss       dword ptr [r13+rbx*4],xmm0
  000000014078C206: movss       xmm0,dword ptr [r15+rbx*4]
  000000014078C20C: ucomiss     xmm0,xmm10
  000000014078C210: setp        al
  000000014078C213: setne       cl
  000000014078C216: or          cl,al
  000000014078C218: or          r14b,cl
  000000014078C21B: mov         eax,1
  000000014078C220: cmove       esi,eax
  000000014078C223: jne         000000014078C22E
  000000014078C225: movss       dword ptr [rbp+0BCh],xmm14
  000000014078C22E: ucomiss     xmm0,xmm11
  000000014078C232: jb          000000014078C120
  000000014078C238: ucomiss     xmm7,xmm0
  000000014078C23B: jbe         000000014078C120
  000000014078C241: subss       xmm0,dword ptr [rbp+0B8h]
  000000014078C249: divss       xmm0,dword ptr [rbp+0E4h]
  000000014078C251: xorps       xmm1,xmm1
  000000014078C254: maxss       xmm1,xmm0
  000000014078C258: movss       xmm8,dword ptr [__real@3f800000]
  000000014078C261: minss       xmm8,xmm1
  000000014078C266: addss       xmm8,dword ptr [__real@bf000000]
  000000014078C26F: mov         rcx,qword ptr [rbp+0C8h]
  000000014078C276: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  000000014078C27B: mov         r14b,al
  000000014078C27E: mov         rcx,qword ptr [rbp+0D8h]
  000000014078C285: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  000000014078C28A: movaps      xmm14,xmm0
  000000014078C28E: pxor        xmm6,xmm6
  000000014078C292: test        r14b,r14b
  000000014078C295: jne         000000014078C29F
  000000014078C297: movd        xmm6,dword ptr [__real@3eeb851f]
  000000014078C29F: andps       xmm8,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  000000014078C2A7: addss       xmm8,xmm8
  000000014078C2AC: divss       xmm8,xmm13
  000000014078C2B1: movss       xmm10,dword ptr [__real@3f800000]
  000000014078C2BA: movaps      xmm0,xmm10
  000000014078C2BE: subss       xmm0,xmm8
  000000014078C2C3: movaps      xmm1,xmm12
  000000014078C2C7: call        powf
  000000014078C2CC: addss       xmm14,xmm6
  000000014078C2D1: movaps      xmm1,xmm10
  000000014078C2D5: subss       xmm1,xmm0
  000000014078C2D9: mulss       xmm1,xmm6
  000000014078C2DD: mulss       xmm14,xmm0
  000000014078C2E2: addss       xmm14,xmm1
  000000014078C2E7: movss       xmm0,dword ptr [r13+rbx*4]
  000000014078C2EE: addss       xmm0,xmm14
  000000014078C2F3: movss       dword ptr [r13+rbx*4],xmm0
  000000014078C2FA: movss       xmm0,dword ptr [r15+rbx*4]
  000000014078C300: ucomiss     xmm0,xmm10
  000000014078C304: setp        al
  000000014078C307: setne       cl
  000000014078C30A: or          cl,al
  000000014078C30C: or          r14b,cl
  000000014078C30F: mov         eax,1
  000000014078C314: cmove       esi,eax
  000000014078C317: jne         000000014078C120
  000000014078C31D: movss       dword ptr [rbp+0BCh],xmm14
  000000014078C326: jmp         000000014078C120
  000000014078C32B: xor         r8d,r8d
  000000014078C32E: jmp         000000014078C4C9
  000000014078C333: xor         esi,esi
  000000014078C335: mov         rdx,qword ptr [rbp+20h]
  000000014078C339: test        rdx,rdx
  000000014078C33C: pcmpeqd     xmm6,xmm6
  000000014078C340: je          000000014078C355
  000000014078C342: mov         rcx,qword ptr [rbp+28h]
  000000014078C346: shl         rdx,3
  000000014078C34A: mov         r8d,4
  000000014078C350: call        __rust_dealloc
  000000014078C355: mov         rdx,qword ptr [rbp+38h]
  000000014078C359: test        rdx,rdx
  000000014078C35C: mov         r13,qword ptr [rbp+0A0h]
  000000014078C363: mov         rbx,qword ptr [rbp+0D0h]
  000000014078C36A: movss       xmm8,dword ptr [__real@c0200000]
  000000014078C373: movss       xmm9,dword ptr [__real@beeb851f]
  000000014078C37C: movss       xmm10,dword ptr [__real@3f75c290]
  000000014078C385: movss       xmm11,dword ptr [__real@3f000000]
  000000014078C38E: movss       xmm13,dword ptr [__real@3fc00000]
  000000014078C397: movss       xmm7,dword ptr [__real@3eeb851f]
  000000014078C39F: movaps      xmm12,xmmword ptr [__xmm@3f0000003f0000003f0000003f000000]
  000000014078C3A7: mov         rdi,qword ptr [rbp+0E8h]
  000000014078C3AE: movss       xmm14,dword ptr [rbp+0BCh]
  000000014078C3B7: je          000000014078C3CB
  000000014078C3B9: shl         rdx,2
  000000014078C3BD: mov         r8d,4
  000000014078C3C3: mov         rcx,r15
  000000014078C3C6: call        __rust_dealloc
  000000014078C3CB: cmp         esi,1
  000000014078C3CE: mov         r15d,dword ptr [rbp+0F4h]
  000000014078C3D5: mov         rcx,qword ptr [rbp+0D8h]
  000000014078C3DC: jne         000000014078C404
  000000014078C3DE: mov         rax,qword ptr [rbp+90h]
  000000014078C3E5: cmp         qword ptr [rax-1B8h],0
  000000014078C3ED: je          000000014078C6DA
  000000014078C3F3: mov         rax,qword ptr [rax-1C0h]
  000000014078C3FA: addss       xmm14,dword ptr [rax]
  000000014078C3FF: movss       dword ptr [rax],xmm14
  000000014078C404: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  000000014078C409: addss       xmm15,xmm0
  000000014078C40E: addss       xmm15,xmm7
  000000014078C413: mov         rax,qword ptr [rbp+90h]
  000000014078C41A: mov         dword ptr [rax-190h],1
  000000014078C424: movss       dword ptr [rax-18Ch],xmm15

; ==================================================================
; place_roof_spires 的 filter_map 闭包 —— 只有脊矩形长宽比 < 1.01（没有脊）才放塔尖
; _ZN4core3ops8function5impls79_$LT$impl$u20$core..ops..function..FnMut$LT$A$GT$$u20$for$u20$$RF$mut$u20$F$GT$8call_mut17h1a78e64a7631eae4E:
  00000001419EE9E0: push        rsi
  00000001419EE9E1: push        rdi
  00000001419EE9E2: sub         rsp,78h
  00000001419EE9E6: movaps      xmmword ptr [rsp+60h],xmm8
  00000001419EE9EC: movaps      xmmword ptr [rsp+50h],xmm7
  00000001419EE9F1: movaps      xmmword ptr [rsp+40h],xmm6
  00000001419EE9F6: mov         rdi,r8
  00000001419EE9F9: mov         rsi,rcx
  00000001419EE9FC: movss       xmm6,dword ptr [r9+4]
  00000001419EEA02: mov         rcx,r8
  00000001419EEA05: call        _ZN12country_core9resources5roofs10roof_shape4Roof2ty17h001241e6e27ea1b3E
  00000001419EEA0A: xor         ecx,ecx
  00000001419EEA0C: mov         edx,0
  00000001419EEA11: test        al,al
  00000001419EEA13: je          00000001419EEB10
  00000001419EEA19: mov         rcx,rdi
  00000001419EEA1C: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims4from17hcb00f4a21c308a29E
  00000001419EEA21: mov         qword ptr [rsp+20h],rax
  00000001419EEA26: lea         rcx,[rsp+20h]
  00000001419EEA2B: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims9get_inner17hb0e1eb6e085c9aa5E
  00000001419EEA30: movd        xmm0,eax
  00000001419EEA34: shr         rax,20h
  00000001419EEA38: movd        xmm1,eax
  00000001419EEA3C: movdqa      xmm2,xmm0
  00000001419EEA40: divss       xmm2,xmm1
  00000001419EEA44: divss       xmm1,xmm0
  00000001419EEA48: movaps      xmm0,xmm2
  00000001419EEA4B: cmpunordss  xmm0,xmm2
  00000001419EEA50: movaps      xmm3,xmm0
  00000001419EEA53: andps       xmm3,xmm1
  00000001419EEA56: maxss       xmm1,xmm2
  00000001419EEA5A: andnps      xmm0,xmm1
  00000001419EEA5D: orps        xmm0,xmm3
  00000001419EEA60: xor         ecx,ecx
  00000001419EEA62: movss       xmm1,dword ptr [__real@3f8147ae]
  00000001419EEA6A: ucomiss     xmm1,xmm0
  00000001419EEA6D: mov         edx,0
  00000001419EEA72: jbe         00000001419EEB10
  00000001419EEA78: lea         rcx,[rsp+34h]
  00000001419EEA7D: mov         rdx,rdi
  00000001419EEA80: call        _ZN12country_core9resources5roofs10roof_shape4Roof8pivot_3d17h4bc65dfe321d2fa5E
  00000001419EEA85: mov         rcx,rdi
  00000001419EEA88: call        _ZN12country_core9resources5roofs10roof_shape4Roof18roof_tip_offset_xz17hd048d6a03455c073E
  00000001419EEA8D: movss       dword ptr [rsp+2Ch],xmm0
  00000001419EEA93: movss       dword ptr [rsp+30h],xmm1
  00000001419EEA99: lea         rcx,[rsp+20h]
  00000001419EEA9E: lea         rdx,[rsp+2Ch]
  00000001419EEAA3: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  00000001419EEAA8: movss       xmm7,dword ptr [rsp+34h]
  00000001419EEAAE: addss       xmm7,dword ptr [rsp+20h]
  00000001419EEAB4: movsd       xmm0,mmword ptr [rsp+38h]
  00000001419EEABA: movsd       xmm8,mmword ptr [rsp+24h]
  00000001419EEAC1: addps       xmm8,xmm0
  00000001419EEAC5: mov         rcx,rdi
  00000001419EEAC8: call        _ZN12country_core9resources5roofs10roof_shape4Roof6height17h927d5fce673ae76fE
  00000001419EEACD: xorps       xmm1,xmm1
  00000001419EEAD0: mulss       xmm1,xmm0
  00000001419EEAD4: addss       xmm7,xmm1
  00000001419EEAD8: unpcklps    xmm0,xmm1
  00000001419EEADB: addps       xmm0,xmm8
  00000001419EEADF: xorps       xmm1,xmm1
  00000001419EEAE2: movss       xmm1,xmm7
  00000001419EEAE6: shufps      xmm1,xmm0,4Ch
  00000001419EEAEA: shufps      xmm1,xmm1,78h
  00000001419EEAEE: movss       xmm0,dword ptr [rdi+2Ch]
  00000001419EEAF3: movaps      xmmword ptr [rsi+10h],xmm1
  00000001419EEAF7: movss       dword ptr [rsi+20h],xmm6
  00000001419EEAFC: movss       dword ptr [rsi+24h],xmm0
  00000001419EEB01: mov         qword ptr [rsi+28h],3E800000h

; ==================================================================
; ui_edit_roof_ridge_dir::switch_roof_ridge_direction —— 只写 ridge_dir，ridge_length_01 不动
  0000000141A3AA2B: movzx       eax,byte ptr [r12+10h]
  0000000141A3AA31: mov         r8d,dword ptr [rsp+34h]
  0000000141A3AA36: mov         dword ptr [rdx+rcx*4],r8d
  0000000141A3AA3A: mov         byte ptr [rbp+r14+3Ch],al
  0000000141A3AA3F: mov         rdi,qword ptr [r12+8]
  0000000141A3AA44: mov         eax,dword ptr [rsp+30h]
  0000000141A3AA48: mov         rcx,qword ptr [rsp+98h]
  0000000141A3AA50: mov         dword ptr [rcx],eax
  0000000141A3AA52: mov         rax,qword ptr [rsp+38h]
  0000000141A3AA57: mov         rbx,qword ptr [rax+30h]
  0000000141A3AA5B: mov         r15,qword ptr [rax+40h]
  0000000141A3AA5F: cmp         rbx,qword ptr [rax+20h]
  0000000141A3AA63: jne         0000000141A3AA76
  0000000141A3AA65: mov         rcx,qword ptr [rsp+60h]
  0000000141A3AA6A: lea         rdx,[anon.84ab5937f659760f8f47d75421f37c6b.1.llvm.9510372700382549680]
  0000000141A3AA71: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h1169a98e92e5dc76E
  0000000141A3AA76: mov         rdx,qword ptr [rsp+38h]
  0000000141A3AA7B: mov         rax,qword ptr [rdx+28h]
  0000000141A3AA7F: mov         rcx,rbx
  0000000141A3AA82: shl         rcx,5
  0000000141A3AA86: mov         qword ptr [rax+rcx],r15
  0000000141A3AA8A: mov         qword ptr [rax+rcx+8],r13
  0000000141A3AA8F: mov         qword ptr [rax+rcx+10h],rdi
  0000000141A3AA94: mov         byte ptr [rax+rcx+18h],1
  0000000141A3AA99: inc         rbx
  0000000141A3AA9C: mov         qword ptr [rdx+30h],rbx
  0000000141A3AAA0: inc         qword ptr [rdx+40h]
  0000000141A3AAA4: mov         r12,qword ptr [r12+8]
  0000000141A3AAA9: mov         eax,dword ptr [rsp+2Ch]
  0000000141A3AAAD: mov         rcx,qword ptr [rsp+90h]
  0000000141A3AAB5: mov         dword ptr [rcx],eax
  0000000141A3AAB7: mov         r15,rsi
  0000000141A3AABA: mov         rbx,qword ptr [rsi+30h]
  0000000141A3AABE: mov         rdi,qword ptr [rsi+40h]
  0000000141A3AAC2: cmp         rbx,qword ptr [rsi+20h]
  0000000141A3AAC6: jne         0000000141A3AAD9
  0000000141A3AAC8: mov         rcx,qword ptr [rsp+58h]
  0000000141A3AACD: lea         rdx,[anon.84ab5937f659760f8f47d75421f37c6b.1.llvm.9510372700382549680]
  0000000141A3AAD4: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h038804b45b640021E
  0000000141A3AAD9: add         rbp,r14
  0000000141A3AADC: mov         rax,qword ptr [r15+28h]
  0000000141A3AAE0: mov         rcx,rbx
  0000000141A3AAE3: shl         rcx,4
  0000000141A3AAE7: mov         qword ptr [rax+rcx],rdi
  0000000141A3AAEB: mov         qword ptr [rax+rcx+8],r12
  0000000141A3AAF0: inc         rbx
  0000000141A3AAF3: mov         qword ptr [r15+30h],rbx
  0000000141A3AAF7: inc         qword ptr [r15+40h]

; ==================================================================
; update_roof_spatial —— 每次墙变把 gable_style 对齐到 is_halftimbered
  00000001421A79AF: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  00000001421A79B4: cmp         byte ptr [r15+54h],al
  00000001421A79B8: mov         r9,r15
  00000001421A79BB: je          00000001421A7A6D
  00000001421A79C1: mov         rcx,rbp
  00000001421A79C4: mov         r15,r9
  00000001421A79C7: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  00000001421A79CC: mov         r9,r15
  00000001421A79CF: mov         ecx,dword ptr [rsp+28h]
  00000001421A79D3: mov         rdx,qword ptr [rsp+40h]
  00000001421A79D8: mov         r8,qword ptr [rsp+48h]
  00000001421A79DD: mov         dword ptr [r8+rdx*4],ecx
  00000001421A79E1: mov         byte ptr [r15+54h],al
  00000001421A79E5: mov         r10,qword ptr [rdi]
  00000001421A79E8: mov         eax,dword ptr [rsp+2Ch]
  00000001421A79EC: mov         rcx,qword ptr [rsp+68h]
  00000001421A79F1: mov         dword ptr [rcx],eax
  00000001421A79F3: mov         rax,qword ptr [rsp+30h]
  00000001421A79F8: mov         rbp,qword ptr [rax+30h]
  00000001421A79FC: mov         r11,qword ptr [rax+40h]
  00000001421A7A00: cmp         rbp,qword ptr [rax+20h]
  00000001421A7A04: jne         00000001421A7A3A
  00000001421A7A06: mov         rcx,qword ptr [rsp+60h]
  00000001421A7A0B: lea         rdx,[anon.84ab5937f659760f8f47d75421f37c6b.1.llvm.9510372700382549680]
  00000001421A7A12: mov         qword ptr [rsp+98h],r10
  00000001421A7A1A: mov         qword ptr [rsp+90h],r11
  00000001421A7A22: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h1169a98e92e5dc76E
  00000001421A7A27: mov         r11,qword ptr [rsp+90h]
  00000001421A7A2F: mov         r10,qword ptr [rsp+98h]
  00000001421A7A37: mov         r9,r15
  00000001421A7A3A: mov         r8,qword ptr [rsp+30h]
  00000001421A7A3F: mov         rcx,qword ptr [r8+28h]
  00000001421A7A43: mov         rdx,rbp
  00000001421A7A46: shl         rdx,5
  00000001421A7A4A: mov         qword ptr [rcx+rdx],r11
  00000001421A7A4E: mov         rax,qword ptr [rsp+38h]
  00000001421A7A53: mov         qword ptr [rcx+rdx+8],rax
  00000001421A7A58: mov         qword ptr [rcx+rdx+10h],r10
  00000001421A7A5D: mov         byte ptr [rcx+rdx+18h],2

; ==================================================================
; wall::shapes::rectangle::create_rectangle —— 新建矩形房子时写进 CreateRoofCmd 的屋顶默认值（三个辅助函数被内联）
  0000000140A93DF7: movss       xmm2,dword ptr [rdi+20h]
  0000000140A93DFC: movss       xmm3,dword ptr [rdi+24h]
  0000000140A93E01: ucomiss     xmm3,xmm2
  0000000140A93E04: seta        r12b
  0000000140A93E08: movaps      xmm1,xmm2
  0000000140A93E0B: divss       xmm1,xmm3
  0000000140A93E0F: movaps      xmm4,xmm3
  0000000140A93E12: divss       xmm4,xmm2
  0000000140A93E16: movaps      xmm0,xmm1
  0000000140A93E19: cmpunordss  xmm0,xmm1
  0000000140A93E1E: movaps      xmm5,xmm0
  0000000140A93E21: andps       xmm5,xmm4
  0000000140A93E24: maxss       xmm4,xmm1
  0000000140A93E28: andnps      xmm0,xmm4
  0000000140A93E2B: orps        xmm0,xmm5
  0000000140A93E2E: movaps      xmm1,xmm2
  0000000140A93E31: cmpunordss  xmm1,xmm2
  0000000140A93E36: movaps      xmm4,xmm1
  0000000140A93E39: andps       xmm4,xmm3
  0000000140A93E3C: maxss       xmm3,xmm2
  0000000140A93E40: andnps      xmm1,xmm3
  0000000140A93E43: orps        xmm1,xmm4
  0000000140A93E46: addss       xmm1,dword ptr [__real@c0100000]
  0000000140A93E4E: movaps      xmm2,xmm1
  0000000140A93E51: divss       xmm2,xmm7
  0000000140A93E55: xorps       xmm3,xmm3
  0000000140A93E58: maxss       xmm3,xmm2
  0000000140A93E5C: movaps      xmm2,xmm15
  0000000140A93E60: minss       xmm2,xmm3
  0000000140A93E64: movaps      xmm3,xmm2
  0000000140A93E67: mulss       xmm3,xmm2
  0000000140A93E6B: addss       xmm2,xmm2
  0000000140A93E6F: movaps      xmm9,xmm6
  0000000140A93E73: subss       xmm9,xmm2
  0000000140A93E78: mulss       xmm9,xmm3
  0000000140A93E7D: mulss       xmm9,xmm10
  0000000140A93E82: addss       xmm9,xmm0
  0000000140A93E87: addss       xmm9,xmm11
  0000000140A93E8C: divss       xmm1,xmm12
  0000000140A93E91: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.48.llvm.3277990281340110158]
  0000000140A93E98: call        _ZN5utils16easing_functions14EasingFunction5apply17hb692303304673b5bE
  0000000140A93E9D: movaps      xmm1,xmm15
  0000000140A93EA1: subss       xmm1,xmm0
  0000000140A93EA5: mulss       xmm1,xmm6
  0000000140A93EA9: mulss       xmm0,xmm13
  0000000140A93EAE: addss       xmm0,xmm1
  0000000140A93EB2: divss       xmm0,xmm14
  0000000140A93EB7: xorps       xmm1,xmm1
  0000000140A93EBA: maxss       xmm1,xmm0
  0000000140A93EBE: movaps      xmm8,xmm15
  0000000140A93EC2: minss       xmm8,xmm1
  0000000140A93EC7: mov         rax,qword ptr [rbp+1E0h]
  0000000140A93ECE: movzx       ebx,byte ptr [rax+41h]
  0000000140A93ED2: cmp         dword ptr [rdi],1
  0000000140A93ED5: jne         0000000140A940A0
  0000000140A93EDB: mov         rsi,qword ptr [rdi+8]
  0000000140A93EDF: mov         qword ptr [rbp+0B0h],rsi
  0000000140A93EE6: mov         rax,qword ptr [rbp+1D8h]
  0000000140A93EED: cmp         qword ptr [rax+48h],0
  0000000140A93EF2: je          0000000140A940A0
  0000000140A93EF8: mov         rcx,qword ptr [rbp+90h]
  0000000140A93EFF: lea         rdx,[rbp+0B0h]
  0000000140A93F06: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  0000000140A93F0B: mov         rdx,qword ptr [rbp+1D8h]
  0000000140A93F12: mov         rcx,qword ptr [rdx+30h]
  0000000140A93F16: mov         rdi,qword ptr [rdx+38h]
  0000000140A93F1A: mov         r8,rdi
  0000000140A93F1D: and         r8,rax
  0000000140A93F20: shr         rax,39h
  0000000140A93F24: movd        xmm0,eax
  0000000140A93F28: punpcklbw   xmm0,xmm0
  0000000140A93F2C: pshuflw     xmm0,xmm0,0
  0000000140A93F31: pshufd      xmm0,xmm0,0
  0000000140A93F36: lea         rax,[rcx-10h]
  0000000140A93F3A: xor         r9d,r9d
  0000000140A93F3D: movdqu      xmm1,xmmword ptr [rcx+r8]
  0000000140A93F43: movdqa      xmm2,xmm1
  0000000140A93F47: pcmpeqb     xmm2,xmm0
  0000000140A93F4B: pmovmskb    r10d,xmm2
  0000000140A93F50: test        r10d,r10d
  0000000140A93F53: je          0000000140A93FA0
  0000000140A93F55: mov         edx,r12d
  0000000140A93F58: mov         r12,r9
  0000000140A93F5B: mov         r9d,ebx
  0000000140A93F5E: tzcnt       r11d,r10d
  0000000140A93F63: add         r11,r8
  0000000140A93F66: and         r11,rdi
  0000000140A93F69: mov         r13,rdi
  0000000140A93F6C: mov         rdi,r11
  0000000140A93F6F: shl         rdi,4
  0000000140A93F73: mov         rbx,rax
  0000000140A93F76: sub         rbx,rdi
  0000000140A93F79: cmp         rsi,qword ptr [rbx]
  0000000140A93F7C: je          0000000140A93FC9
  0000000140A93F7E: lea         r11d,[r10-1]
  0000000140A93F82: and         r11w,r10w
  0000000140A93F86: mov         r10d,r11d
  0000000140A93F89: mov         ebx,r9d
  0000000140A93F8C: mov         r9,r12
  0000000140A93F8F: mov         r12d,edx
  0000000140A93F92: mov         rdi,r13
  0000000140A93F95: jne         0000000140A93F55
  0000000140A93F97: nop         word ptr [rax+rax]
  0000000140A93FA0: pcmpeqb     xmm1,xmmword ptr [__xmm@ffffffffffffffffffffffffffffffff]
  0000000140A93FA8: pmovmskb    r10d,xmm1
  0000000140A93FAD: test        r10d,r10d
  0000000140A93FB0: jne         0000000140A940A0
  0000000140A93FB6: add         r8,r9
  0000000140A93FB9: add         r8,10h
  0000000140A93FBD: add         r9,10h
  0000000140A93FC1: and         r8,rdi
  0000000140A93FC4: jmp         0000000140A93F3D
  0000000140A93FC9: neg         r11
  0000000140A93FCC: shl         r11,4
  0000000140A93FD0: mov         rcx,qword ptr [rcx+r11-8]
  0000000140A93FD5: mov         rax,rcx
  0000000140A93FD8: shr         rax,20h
  0000000140A93FDC: mov         ebx,r9d
  0000000140A93FDF: jne         0000000140A93FE9
  0000000140A93FE1: mov         r12d,edx
  0000000140A93FE4: jmp         0000000140A940A0
  0000000140A93FE9: mov         ecx,ecx
  0000000140A93FEB: mov         r10,qword ptr [rbp+0A0h]
  0000000140A93FF2: cmp         rcx,qword ptr [r10+10h]
  0000000140A93FF6: mov         r12d,edx
  0000000140A93FF9: jae         0000000140A940A0
  0000000140A93FFF: mov         rdx,qword ptr [r10+8]
  0000000140A94003: lea         rcx,[rcx+rcx*4]
  0000000140A94007: cmp         dword ptr [rdx+rcx*4],eax
  0000000140A9400A: jne         0000000140A940A0
  0000000140A94010: lea         rax,[rdx+rcx*4]
  0000000140A94014: mov         ecx,dword ptr [rax+4]
  0000000140A94017: mov         edx,0FFFFFFFFh
  0000000140A9401C: cmp         rcx,rdx
  0000000140A9401F: je          0000000140A940A0
  0000000140A94025: mov         r9,qword ptr [rbp+0A8h]
  0000000140A9402C: cmp         qword ptr [r9+38h],rcx
  0000000140A94030: jbe         0000000140A940A0
  0000000140A94032: mov         rdx,qword ptr [r9+28h]
  0000000140A94036: mov         r8d,ecx
  0000000140A94039: shr         r8d,6
  0000000140A9403D: mov         rdx,qword ptr [rdx+r8*8]
  0000000140A94041: bt          rdx,rcx
  0000000140A94045: jae         0000000140A940A0
  0000000140A94047: mov         ecx,dword ptr [rax+0Ch]
  0000000140A9404A: mov         eax,dword ptr [rax+10h]
  0000000140A9404D: mov         rdx,qword ptr [r10+1A8h]
  0000000140A94054: lea         rcx,[rcx+rcx*8]
  0000000140A94058: mov         r8,qword ptr [r9+110h]
  0000000140A9405F: mov         r9,qword ptr [rdx+rcx*8+18h]
  0000000140A94064: mov         rcx,qword ptr [rdx+rcx*8+38h]
  0000000140A94069: mov         rcx,qword ptr [rcx+r8*8]
  0000000140A9406D: not         rcx
  0000000140A94070: lea         rcx,[rcx+rcx*2]
  0000000140A94074: shl         rcx,4
  0000000140A94078: mov         rcx,qword ptr [r9+rcx+10h]
  0000000140A9407D: imul        rax,rax,58h
  0000000140A94081: movzx       ebx,byte ptr [rcx+rax+3Dh]
  0000000140A94086: movss       xmm0,dword ptr [rcx+rax+30h]
  0000000140A9408C: movss       xmm1,dword ptr [__real@3dcccccd]
  0000000140A94094: ucomiss     xmm1,xmm0
  0000000140A94097: jb          0000000140A940A0
  0000000140A94099: movaps      xmm8,xmm0
  0000000140A9409D: nop         dword ptr [rax]
  0000000140A940A0: mov         eax,dword ptr [rbp+1C0h]
  0000000140A940A6: mov         rcx,qword ptr [rbp+10h]
  0000000140A940AA: mov         dword ptr [rcx],eax
  0000000140A940AC: mov         rax,qword ptr [rbp+150h]
  0000000140A940B3: mov         rsi,qword ptr [rax+30h]
  0000000140A940B7: mov         rdi,qword ptr [rax+40h]
  0000000140A940BB: cmp         rsi,qword ptr [rax+20h]
  0000000140A940BF: jne         0000000140A940D1
  0000000140A940C1: mov         rcx,qword ptr [rbp+70h]
  0000000140A940C5: lea         rdx,[anon.60360aed504c4a0f85b3886feb406ce0.1.llvm.17420070914341975340]
  0000000140A940CC: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h0217977ba397d77cE
  0000000140A940D1: divss       xmm9,xmm10
  0000000140A940D6: xorps       xmm0,xmm0
  0000000140A940D9: maxss       xmm0,xmm9
  0000000140A940DE: movaps      xmm1,xmm15
  0000000140A940E2: minss       xmm1,xmm0
  0000000140A940E6: movaps      xmm0,xmm1
  0000000140A940E9: mulss       xmm0,xmm1
  0000000140A940ED: addss       xmm1,xmm1
  0000000140A940F1: movaps      xmm9,xmm6
  0000000140A940F5: subss       xmm9,xmm1
  0000000140A940FA: mulss       xmm9,xmm0
  0000000140A940FF: mov         r8,qword ptr [rbp+150h]
  0000000140A94106: mov         rax,qword ptr [r8+28h]
  0000000140A9410A: mov         rcx,rsi
  0000000140A9410D: shl         rcx,6
  0000000140A94111: mov         qword ptr [rax+rcx],rdi
  0000000140A94115: mov         qword ptr [rax+rcx+8],r15
  0000000140A9411A: mov         edx,dword ptr [rbp+1F8h]
  0000000140A94120: mov         dword ptr [rax+rcx+10h],edx
  0000000140A94124: mov         rdx,qword ptr [rbp+128h]
  0000000140A9412B: mov         qword ptr [rax+rcx+14h],rdx
  0000000140A94130: mov         edx,dword ptr [rbp+130h]
  0000000140A94136: mov         dword ptr [rax+rcx+1Ch],edx
  0000000140A9413A: mov         qword ptr [rax+rcx+20h],0
  0000000140A94143: mov         dword ptr [rax+rcx+28h],3F000000h
  0000000140A9414B: movss       dword ptr [rax+rcx+2Ch],xmm8
  0000000140A94152: movss       dword ptr [rax+rcx+30h],xmm9
  0000000140A94159: mov         dword ptr [rax+rcx+34h],3E4CCCCDh
  0000000140A94161: mov         byte ptr [rax+rcx+38h],r12b
  0000000140A94166: mov         byte ptr [rax+rcx+39h],bl
  0000000140A9416A: mov         byte ptr [rax+rcx+3Ch],0

; ==================================================================
; utils::trim_rows —— 山墙的行端吸附 0.2 块砖、不足一块砖的行段加宽
  000000014123F249: movzx       r15d,byte ptr [rbp+130h]
  000000014123F251: test        r15b,r15b
  000000014123F254: je          000000014123F25F
  000000014123F256: movss       xmm8,dword ptr [__real@3e4ccccd]
  000000014123F25F: mov         rbx,qword ptr [rbp+20h]
  000000014123F263: test        rbx,rbx
  000000014123F266: je          000000014123F3B9
  000000014123F26C: movss       xmm6,dword ptr [__real@3eeb851f]
  000000014123F274: divss       xmm6,xmm7
  000000014123F278: mulss       xmm8,xmm6
  000000014123F27D: mov         rsi,qword ptr [rbp+18h]
  000000014123F281: movss       xmm9,dword ptr [__real@3f800000]
  000000014123F28A: movaps      xmm10,xmm9
  000000014123F28E: divss       xmm10,xmm8
  000000014123F293: mov         rdi,rbx
  000000014123F296: shl         rdi,5
  000000014123F29A: xor         r14d,r14d
  000000014123F29D: xorps       xmm11,xmm11
  000000014123F2A1: jmp         000000014123F2B9
  000000014123F2A3: nop         word ptr cs:[rax+rax]
  000000014123F2B0: add         r14,20h
  000000014123F2B4: cmp         rdi,r14
  000000014123F2B7: je          000000014123F323
  000000014123F2B9: movss       xmm0,dword ptr [rsi+r14+18h]
  000000014123F2C0: ucomiss     xmm0,xmm11
  000000014123F2C4: jne         000000014123F2C8
  000000014123F2C6: jnp         000000014123F2ED
  000000014123F2C8: divss       xmm0,xmm8
  000000014123F2CD: call        roundf
  000000014123F2D2: mulss       xmm0,xmm8
  000000014123F2D7: xorps       xmm1,xmm1
  000000014123F2DA: maxss       xmm1,xmm0
  000000014123F2DE: movaps      xmm0,xmm9
  000000014123F2E2: minss       xmm0,xmm1
  000000014123F2E6: movss       dword ptr [rsi+r14+18h],xmm0
  000000014123F2ED: movss       xmm0,dword ptr [rsi+r14+1Ch]
  000000014123F2F4: ucomiss     xmm0,xmm9
  000000014123F2F8: jne         000000014123F2FC
  000000014123F2FA: jnp         000000014123F2B0
  000000014123F2FC: mulss       xmm0,xmm10
  000000014123F301: call        roundf
  000000014123F306: mulss       xmm0,xmm8
  000000014123F30B: xorps       xmm1,xmm1
  000000014123F30E: maxss       xmm1,xmm0
  000000014123F312: movaps      xmm0,xmm9
  000000014123F316: minss       xmm0,xmm1
  000000014123F31A: movss       dword ptr [rsi+r14+1Ch],xmm0
  000000014123F321: jmp         000000014123F2B0
  000000014123F323: test        r15b,r15b
  000000014123F326: je          000000014123F3CB
  000000014123F32C: test        rbx,rbx
  000000014123F32F: je          000000014123F6CF
  000000014123F335: movss       xmm0,dword ptr [__real@3f000000]
  000000014123F33D: mulss       xmm0,xmm6
  000000014123F341: xor         eax,eax
  000000014123F343: movss       xmm1,dword ptr [__real@3f000000]
  000000014123F34B: jmp         000000014123F35D
  000000014123F34D: nop         dword ptr [rax]
  000000014123F350: add         rax,20h
  000000014123F354: cmp         rdi,rax
  000000014123F357: je          000000014123F4B0
  000000014123F35D: movss       xmm3,dword ptr [rsi+rax+18h]
  000000014123F363: movss       xmm2,dword ptr [rsi+rax+1Ch]
  000000014123F369: movaps      xmm4,xmm2
  000000014123F36C: subss       xmm4,xmm3
  000000014123F370: ucomiss     xmm6,xmm4
  000000014123F373: jb          000000014123F350
  000000014123F375: addss       xmm3,xmm2
  000000014123F379: mulss       xmm3,xmm1
  000000014123F37D: subss       xmm3,xmm0
  000000014123F381: xorps       xmm4,xmm4
  000000014123F384: maxss       xmm4,xmm3
  000000014123F388: movaps      xmm3,xmm9
  000000014123F38C: minss       xmm3,xmm4
  000000014123F390: movss       dword ptr [rsi+rax+18h],xmm3
  000000014123F396: addss       xmm3,xmm2
  000000014123F39A: mulss       xmm3,xmm1
  000000014123F39E: addss       xmm3,xmm0
  000000014123F3A2: xorps       xmm2,xmm2
  000000014123F3A5: maxss       xmm2,xmm3
  000000014123F3A9: movaps      xmm3,xmm9
  000000014123F3AD: minss       xmm3,xmm2
  000000014123F3B1: movss       dword ptr [rsi+rax+1Ch],xmm3
  000000014123F3B7: jmp         000000014123F350
