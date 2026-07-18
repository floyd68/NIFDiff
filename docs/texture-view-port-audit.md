# FICTure2 → NIFDiff Texture View/Diff 포팅 감사

## 1. 범위와 결론

- 검토 범위: `9271028..0f3639c` (`ac80ef7`부터 시작된 Texture View/Diff 포팅 포함)
- 비교 대상: `D:\Works\FICTure2`
- 검토 항목: 기능 parity, 비동기 수명, device-loss, 코드 중복, 구조적 bloat

`ComparePane`/`PaneContent` 기반 혼합 NIF·이미지 pane 구조와 기본 Texture View/Diff
흐름은 타당하다. 그러나 아카이브 이미지 로딩, 비동기 최신성, GPU fallback,
graphics generation 처리에 실제 오동작 가능성이 높은 문제가 남아 있다. 현재 상태를
production-ready 포팅 완료로 보기는 어렵다.

권장 순서는 다음과 같다.

1. P0 정확성 문제를 먼저 수정한다.
2. FICTure2의 검증된 async binding/upload 계층을 공용 컴포넌트로 추출한다.
3. `ThumbnailStrip`의 NIF/image thumbnail 작업을 별도 provider로 분리한다.
4. path 분류를 중앙화하고 자동 회귀 테스트를 추가한다.
5. 이후 context menu, screenshot, sampling, info panel 등의 parity를 채운다.

## 2. 검증 결과

### 통과

- Release `NIFDiff`, `ImageViewTest` 빌드
- Debug `NIFDiff` 빌드
- Release `NifValidate`, `ResourceResolveTest` 빌드
- `ResourceResolveTest` 실행
  - override → NIF directory → Game Data 순서
  - texture prefix 보정
  - 실제 BSA/BA2 추출
  - loose resource 탐색
- `git diff 9271028..HEAD --check`

### 검증 공백

`ctest --test-dir build -C Release`에는 등록된 테스트가 없었다.
`ImageViewTest`는 수동 harness이며 다음 경로를 자동 검증하지 않는다.

- archive member 이미지/thumbnail
- 빠른 연속 선택 중 stale completion
- D3D upload 실패 후 CPU fallback
- renderer fallback과 device recreation
- pane kind 전환과 open routing
- 실패한 decode의 MRU/session 처리
- CPU bitmap 경로의 channel isolation

## 3. P0 정확성 문제

### 3.1 아카이브 내부 이미지가 decode되지 않음

**근거**

- `app/NIFDiffApp.cpp:732-735`는 `ImageCore::RegisterBuiltInDecoders()`만 호출한다.
- NIFDiff 어디에서도 `ImageCore::SetPathByteSource()`를 호출하지 않는다.
- `third_party/ImageCore/DecodeScheduler.cpp:121-155`는 archive-backed path를
  읽기 위해 `IPathByteSource`를 요구한다.
- FICTure2는 `FICture2.cpp:245-246`에서 Floar adapter를 등록한다.

**영향**

`ThumbnailStrip`은 Floar로 BSA/BA2/ZIP/7z/RAR 내부 이미지를 목록에 넣지만,
ImageCore는 선택된 virtual path를 filesystem path로 처리한다. 따라서
`ImagePane`의 full-resolution decode와 `ThumbnailStrip`의 image thumbnail decode가
실패한다. README의 archive 지원 설명과 실제 동작도 일치하지 않는다.

**권장 수정**

- FICTure2의 `FloarPathByteSource`를 NIFDiff app 계층으로 옮긴다.
- 첫 ImageCore 요청 전에 `SetPathByteSource()`로 등록한다.
- shutdown 시 source를 해제한다.
- archive image와 archive thumbnail을 자동 테스트한다.

### 3.2 이전 decode가 더 새 이미지 위에 덮일 수 있음

**근거**

`ui/ImagePane.cpp:481-489`의 callback은 generation을 검사한 뒤
`ImageView::StagePayload()`를 호출한다. 그러나 검사와 staging은 원자적이지 않다.

가능한 순서:

1. 이전 callback이 generation 검사를 통과한다.
2. UI가 새 path를 선택해 generation을 증가시킨다.
3. 새 path가 GPU cache hit로 즉시 표시된다.
4. 이전 callback이 뒤늦게 payload를 stage한다.
5. 다음 frame에서 이전 이미지가 새 이미지 위에 upload된다.

**영향**

thumbnail을 빠르게 이동하거나 cache hit와 decode completion이 겹치면 현재 path와
다른 이미지가 표시될 수 있다.

**권장 수정**

- FICTure2의 `ImageAsyncBinding`처럼 path와 generation을 공유 lock 아래에서
  재검사한다.
- 또는 staged payload에 path/generation을 넣고 upload 직전에도 다시 검증한다.
- old completion after cache hit 순서를 재현하는 테스트를 추가한다.

### 3.3 GPU upload 실패 또는 renderer fallback에서 DDS가 blank가 됨

**근거**

- `ui/ImagePane.cpp:472-477`은 D3D 가용 여부와 무관하게
  `allowGpuCompressedDDS = true`를 설정한다.
- compressed payload는 `OnRenderD3D()`만 처리한다.
- `ui/ImagePane.cpp:130-168`은 texture/SRV 생성 전에 pending flag를 소비한다.
- `CreateTexture2D`/`CreateShaderResourceView` 실패 후 retry나 CPU decode가 없다.
- typeless BC format은 명시적인 typed SRV가 필요할 수 있다.

**영향**

- FD2D가 D2D-only fallback에 들어가면 compressed DDS가 표시되지 않는다.
- GPU resource 생성 실패 시 같은 payload를 다시 시도하지 않는다.
- decode는 성공했지만 pane은 계속 blank로 남을 수 있다.

FICTure2의 `ImageBrowserMainImage.cpp:493-520,575-653`은 D3D가 없으면 CPU decode를
선택하고 GPU upload 실패 후에도 CPU BGRA decode를 다시 요청한다.

**권장 수정**

- D3D device가 없으면 `allowGpuCompressedDDS = false`로 요청한다.
- GPU texture/SRV 생성 실패 시 CPU BGRA decode로 재시도한다.
- upload 성공 후에만 pending 상태를 소비한다.
- typeless BC format에는 명시적인 typed SRV format을 사용한다.

### 3.4 `ThumbnailStrip`이 죽은 graphics generation을 유지함

**근거**

- `ui/ThumbnailStrip.cpp:159-164`는 attach 시 raw device/context를 저장한다.
- `ThumbnailStrip`에는 `OnGraphicsInvalidated` override가 없다.
- `Entry::tex`, `Entry::bitmap`, `m_thumbTarget`, `m_thumbCache`가 그대로 유지된다.
- 이후 `ui/ThumbnailStrip.cpp:693-736` 등에서 이전 device/context를 사용한다.

**영향**

- target recreation 후 `D2DERR_WRONG_RESOURCE_DOMAIN`
- 이전 device texture를 새 context에서 사용하는 cross-device 오류
- raw context pointer의 dangling access 가능성

port 전부터 존재한 `NifViewport`/`RenderDevice`의 generation 처리도 함께 점검해야
한다. `NifViewport::OnAttached`는 device를 한 번만 연결하고,
`RenderDevice::EnsureInitialized`는 기존 device가 있으면 즉시 반환한다.

**권장 수정**

- invalidation 시 device/context, bitmap, texture, render target, mesh cache를 해제한다.
- 다음 attach/render에서 현재 generation의 device/context를 다시 얻는다.
- device recreation 테스트에서 NIF pane과 image strip을 함께 검증한다.

## 4. P1 기능 오류와 불완전한 동작

### 4.1 실패한 image load가 성공으로 기록됨

`ui/ImagePane.cpp:440-493`은 decode 완료 전에 path/label을 바꾸고 view를 reset하며
`NotifyFileOpened(path)`를 호출한 뒤 `true`를 반환한다. callback은 decode 실패를
무시하고 기존 화면도 clear하지 않는다.

그 결과 손상된 파일명 아래 이전 이미지가 계속 보이고 실패한 path가 MRU/session에
들어간다. `SetSource`, `ReloadCurrentSource`, `OnLoadSucceeded`, `OnLoadFailed`를
분리하고 실제 성공 후에만 open notification을 보내야 한다.

### 4.2 device recovery가 사용자 open 동작을 다시 실행함

cache-served SRV가 device loss로 사라지면 `Load(m_path)`를 다시 호출한다. 이 경로는
zoom/rotation/channel을 reset하고 MRU를 다시 갱신한다. 사용자 source 변경과 graphics
recovery reload를 분리하고 recovery에서는 view state와 MRU를 보존해야 한다.
`Shutdown` invalidation에서는 reload하지 않아야 한다.

### 4.3 archive member session이 restore되지 않음

`app/NIFDiffApp.cpp:444-455`의 `GatherInitialPaths()`는 저장 path를
`GetFileAttributesW()`로 검사한다. Floar archive member/subdirectory display path는
일반 filesystem path가 아니므로 제거된다.

`VirtualPath`/`VirtualFileSystem`으로 존재 여부를 검사하고, 필요하면 현재 표시 파일과
browse container를 session에 따로 저장해야 한다.

### 4.4 composition 변경으로 NIF screenshot menu가 사라짐

`app/NIFDiffApp.cpp:181`:

```cpp
NifComparePane* nifPane = dynamic_cast<NifComparePane*>(pane);
```

`pane`은 `ComparePane*`이며 `NifComparePane`의 base가 아니므로 항상 실패한다.
`pane->NifContent()`를 사용해야 한다. F12 경로는 `AsNif()`를 사용하므로 별도로
동작한다. FICTure2 parity를 위해 image pane screenshot도 추가할 수 있다.

### 4.5 CPU bitmap 이미지에서 channel isolation이 동작하지 않음

BC DDS는 `SetShaderResource()` 경로를 사용하지만 PNG/JPEG/TGA와 non-BC DDS는
`SetBitmap()` 경로를 사용한다. FD2D의 `channelMode`는 SRV shader 경로에서만
처리되므로 R/G/B/A 단축키가 advertised format 중 상당수에서 아무 효과가 없다.

full-resolution 이미지를 SRV로 upload하는 단일 presentation path가 가장 단순하다.
bitmap path를 유지한다면 동일 channel transform을 따로 구현해야 한다.

### 4.6 image thumbnail decode가 worker를 이중 사용함

`ui/ThumbnailStrip.cpp:356-386`은 `ResourceManager` worker 안에서 ImageCore async
request를 시작한 뒤 `future::get()`으로 block한다. ImageCore handle을 보관하지 않아
folder generation이 바뀌어도 실제 decode를 cancel할 수 없다.

image thumbnail은 ImageCore에 직접 제출하고 generation별 handle을 보관해야 한다.
completion만 UI 또는 ResourceManager completion queue로 marshal하는 편이 낫다.

## 5. FICTure2 기능 parity

### 구현된 핵심 기능

- ImageCore 기반 DDS/WIC format decode
- image/NIF/container 종류에 따른 pane routing
- 동일 pane에서 NIF ↔ image content 교체
- 최대 8개 mixed pane compare layout
- 기본 zoom, pan, 90도 rotation
- SRV 경로의 R/G/B/A channel isolation
- local alpha checkerboard
- per-pane thumbnail strip과 image thumbnail
- folder/archive navigation
- replace/insert drag-and-drop
- command line, dialog, MRU, IPC 진입점
- 기본 session path와 split ratio 저장
- BCn mip 0 직접 GPU upload
- path → SRV LRU cache

### 누락 또는 부분 포팅

1. **Image context menu**
   - fit, sampling, alpha, rotate 180/reset, background, directory visibility,
     screenshot 명령이 없다.

2. **Image screenshot/export**
   - 현재 screenshot은 NIF viewport 전용이며 F12도 image pane에서는 동작하지 않는다.

3. **Sampling quality toggle**
   - `ImagePane::PushDrawState()`가 high-quality sampling을 항상 `true`로 고정한다.

4. **Image metadata/info panel**
   - dimensions, DXGI format, bpp, mip, sampling renderer, archive source 표시가 없다.

5. **Loading/error UI**
   - FICTure2의 spinner와 failure state가 포팅되지 않았다.

6. **Smooth zoom**
   - FICTure2의 spring zoom과 stiffness 설정이 없고 즉시 zoom만 한다.

7. **Pan 제약과 capture**
   - 이미지가 완전히 화면 밖으로 나가지 않도록 하는 clamp와 mouse capture가 없다.

8. **Rotation 세부 기능**
   - 180도 회전, rotation-only reset, 현재 angle 표시가 없다.

9. **Directory tile visibility**
   - thumbnail strip의 folder/`..` 항목을 숨기는 기능이 없다.

10. **Background color**
    - pane/focused-pane background 설정과 동기화가 없다.

11. **Alpha checkerboard sync**
    - local toggle만 있고 cross-pane global sync는 없다.

12. **View sync 의미 차이**
    - FICTure2는 같은 filename을 표시하는 pane 사이에서만 transform을 sync한다.
    - NIFDiff는 filename과 무관하게 모든 image pane에 transform을 적용한다.

13. **Filename sync 의미 차이**
    - FICTure2는 EventBus selection sync를 사용한다.
    - NIFDiff는 “Sync Files” 아래에서 다른 pane folder의 같은 basename을 load한다.

14. **Thumbnail Page Up/Page Down**
    - FICTure2는 page 단위 이동을 제공하지만 NIFDiff의 PgUp/PgDn은 NIF camera
      preset에 사용된다.

15. **Image file associations / Explorer thumbnail provider**
    - NIFDiff shell integration은 `.nif` 중심이다.

16. **Title bar active filename — 완료**
    - 활성 pane이 바뀌거나 해당 pane의 파일/container가 바뀌면
      `<filename> - NIFDiff <version> - NIF Model Compare`로 갱신된다.
    - 활성 pane이 비어 있으면 제품명과 버전만 있는 기본 title로 복원된다.

### 두 프로젝트가 공유하는 현재 제한

- full-resolution DDS는 mip 0만 표시
- cubemap은 첫 face만 표시
- mip/face 선택 UI 없음
- hardware D3D 생성 실패 시 WARP fallback 없음

이 항목은 이번 포팅 regression은 아니지만 FICTure2 이상의 완전한 texture viewer를
목표로 한다면 별도 backlog로 관리해야 한다.

## 6. 코드 bloat와 중복 구현

### 6.1 `ImagePane.cpp`가 검증된 bridge를 다시 구현함

`ui/ImagePane.cpp`의 nested `ImageView`는 다음 책임을 다시 구현한다.

- async request generation과 cancellation
- payload staging
- BC format 분류
- D3D texture/SRV upload
- D2D bitmap upload
- GPU cache
- device-loss recovery
- zoom/pan/rotation/channel 입력 상태

FICTure2에는 이미 `ImageAsyncBinding`, `ImageBrowserMainImage`,
`ImageGpuResourceCache`에 검증된 구현이 있으며 CPU fallback, failure state,
경로 정규화, generation 재검사 등이 더 완전하다.

단순 코드 중복보다 중요한 문제는 재구현 과정에서 안전장치가 빠졌다는 점이다.
renderer 독립적인 ImageCore 자체에 넣기보다는 FD2D와 ImageCore 사이의 공용
presentation/binding 계층으로 추출하는 것이 적절하다.

공용 계층의 책임:

- source/generation/cancellation state
- normalized source key
- pending payload와 loading/failure 상태
- SRV/D2D upload와 CPU fallback
- device-generation invalidation
- 공용 view transform

NIFDiff와 FICTure2는 이 계층을 공유하고 각 앱은 pane chrome, command, sync policy만
소유하는 구성이 바람직하다.

### 6.2 `ImageGpuResourceCache`는 사실상 복사본

`ui/ImageGpuResourceCache.*`는 FICTure2의 같은 클래스와 거의 동일하다.
작은 코드이므로 당장 큰 bloat는 아니지만 두 앱에서 따로 수정되면 device-loss,
normalization, eviction 정책이 다시 갈라질 수 있다. 위 공용 presentation 계층에
포함시키는 편이 낫다.

### 6.3 `ThumbnailStrip`의 책임이 과도함

`ThumbnailStrip`은 약 1,300줄 규모이며 다음을 모두 담당한다.

- Floar directory/archive enumeration
- NIF/image path 분류
- NIF parse/build
- offscreen NIF thumbnail render
- image thumbnail async decode
- D2D/D3D resource 생성
- layout, scrolling, resize
- keyboard/type-to-select/input
- generation과 completion 처리

권장 분리:

- `ThumbnailStrip`: presentation, selection, navigation, layout
- `ThumbnailSource`: VFS enumeration과 entry model
- `NifThumbnailProvider`: parse/build/offscreen render
- `ImageThumbnailProvider`: ImageCore request/cancel/completion

### 6.4 image path 분류 정책이 중복됨

유사한 판정이 다음 위치에 있다.

- `ui/ComparePane.cpp::PathIsImage`
- `ui/NifCompareView.cpp`의 image/NIF/container helper
- `ui/ThumbnailStrip.cpp::IsImageExt`
- `app/FileDialog.cpp`의 filter 구성

알 수 없는 파일은 일부 경로에서 NIF로 간주된다. “All files”로 선택한 임의 파일이
NIF parser로 전달되어도 오류가 사용자에게 전달되지 않는다.

공용 `OpenPathClassifier`를 추가해 `Nif | Image | Container | Unsupported`를
반환하게 하고 dialog, drag/drop, command line, IPC, session restore, thumbnail
listing이 같은 정책을 사용해야 한다. `Load()`의 bool 대신 structured result를
사용하는 것도 권장한다.

### 6.5 NIF texture inspector가 별도 synchronous decode를 수행함

`ui/NifCompareView.cpp:1525-1618`의 `EnsureTexturePreview()`는 resource를 다시 찾고
DirectXTex로 DDS를 다시 load/decompress/convert한 뒤 D2D bitmap을 만든다.
`TextureRepository`와 ImageCore가 수행하는 decode와 별개이며 UI/render 경로에서
동기 실행된다.

inspector preview를 async service로 분리하고 ImageCore의 preview purpose 또는
repository가 보유한 resolved source를 재사용하는 편이 낫다. channel transform도
FD2D SRV shader path를 공유할 수 있다.

### 6.6 `ImageViewTest`의 현재 역할

`app/ImageViewTest.cpp`는 초기 통합을 증명하기 위한 157줄 수동 harness이다.
현재 app path와 중복된 bitmap upload를 구현하지만 실제 회귀 테스트는 아니다.

- 자동화 가능하면 archive/format/failure fixture 기반 test로 전환한다.
- 수동 도구로 유지하면 기본 `ALL` build에서 제외한다.
- app과 같은 공용 presentation 계층을 사용해 production 경로를 검증하게 한다.

## 7. 유지할 가치가 있는 설계

### `ComparePane` + swappable `PaneContent`

- thumbnail callback 실행 중 strip 자체를 파괴하지 않는다.
- NIF ↔ image 전환 후에도 folder, scroll, selection 상태를 보존한다.
- mixed pane compare라는 NIFDiff 고유 요구에 잘 맞는다.

### 하나의 split coordinator

NIF와 image가 같은 layout 규칙을 사용하므로 image 전용 coordinator는 필요 없다.
기존 `NifCompareSplitCoordinator`를 mixed pane에 재사용한 것은 적절하다.

### `TextureRepository`와 standalone image cache 분리

두 cache의 역할은 다르다.

- `TextureRepository`
  - Bethesda resource resolution 결과를 key로 사용
  - NIF material texture 공유
  - mip/cube metadata와 complex-material probe
  - async prefetch와 engine search order
- standalone image cache
  - 사용자가 직접 연 display source를 key로 사용
  - image viewer의 빠른 재선택

따라서 두 서비스를 억지로 합치지 않는 것이 좋다. 다만 standalone image cache의
구현은 FICTure2와 공유할 수 있다.

### NIF와 image decode pipeline의 완전 통합은 불필요

NIF material texture는 Bethesda search order와 archive resolution이 필요하지만
standalone image open은 명시적 source를 표시한다. 공통 upload/binding 계층은
공유하되 source resolution policy는 분리해야 한다.

## 8. 권장 구현 순서

### 단계 1: correctness hotfix

1. Floar `IPathByteSource` 등록
2. staged payload의 path/generation 이중 검사
3. D3D 미사용 및 upload 실패 시 CPU fallback
4. `ThumbnailStrip` graphics invalidation
5. 실패한 decode의 loading/error/MRU 처리
6. archive member session restore
7. `ComparePane::NifContent()`를 사용하도록 screenshot cast 수정
8. CPU image channel isolation 수정

이 단계에서는 기능을 추가하기보다 잘못된 이미지, blank pane, archive 실패,
device recreation 오류를 제거하는 데 집중한다.

### 단계 2: 공용 image presentation 계층

FICTure2의 `ImageAsyncBinding`과 현재 `ImagePane::ImageView`를 기준으로 공용 계층을
만들고 두 앱이 사용하도록 한다.

필수 기능:

- lifetime-safe async binding
- source normalization
- stale completion 차단
- loading/failure state
- compressed DDS GPU upload
- CPU bitmap/SRV fallback
- device-generation-aware cache
- device-loss reupload/redecode
- zoom/pan/rotation/channel draw state

### 단계 3: thumbnail 구조 분리

`ThumbnailStrip`에서 VFS model과 NIF/image provider를 분리한다. image thumbnail은
ResourceManager worker를 block하지 않고 ImageCore에 직접 요청한다.

### 단계 4: path/open 정책 통합

하나의 path classifier와 structured open result를 도입한다. 모든 open entry point가
같은 지원 형식, 오류 처리, archive semantics를 사용하게 한다.

### 단계 5: 자동 테스트

최소 테스트 목록:

1. loose PNG/JPEG/TGA/DDS decode
2. BSA/BA2/ZIP 내부 image와 thumbnail decode
3. corrupt image의 실패 상태와 MRU 미기록
4. A decode 중 B cache hit 발생 후 A completion
5. typeless BC DDS의 GPU 실패 → CPU fallback
6. D2D-only renderer fallback
7. target recreation과 full device recreation
8. image ↔ NIF content 반복 전환
9. archive member session save/restore
10. CPU/GPU 양쪽 path의 R/G/B/A isolation
11. thumbnail folder 전환 중 request cancellation
12. screenshot context menu의 NIF/image 분기

### 단계 6: 기능 parity

correctness와 구조 정리 후 다음 순서가 효율적이다.

1. image screenshot과 context menu
2. sampling toggle과 info panel
3. loading spinner/error 표시
4. filename-gated transform sync 정책
5. rotate 180/reset, smooth zoom, pan clamp
6. directory visibility/background/association 등의 app polish

## 9. 주요 파일

- `app/NIFDiffApp.cpp`
  - ImageCore 초기화/종료
  - session restore
  - context menu와 screenshot
- `ui/ImagePane.h/.cpp`
  - async full-image loading
  - payload upload와 view interaction
- `ui/ImageGpuResourceCache.h/.cpp`
  - standalone image SRV LRU
- `ui/ComparePane.h/.cpp`
  - persistent frame와 swappable content
  - path kind routing
- `ui/PaneContent.h`
  - mixed pane abstraction
- `ui/ThumbnailStrip.h/.cpp`
  - VFS navigation, NIF/image thumbnail, layout/input
- `ui/NifCompareView.h/.cpp`
  - cross-pane sync
  - open entry point
  - NIF texture inspector
- `third_party/ImageCore`
  - decode scheduler와 `IPathByteSource`
- `third_party/FD2D/Image.h/.cpp`
  - bitmap/SRV presentation과 shader channel mode
- FICTure2 참고 구현
  - `FloarPathByteSource.h/.cpp`
  - `ImageAsyncBinding.h/.cpp`
  - `ImageBrowserMainImage.h/.cpp`
  - `ImageGpuResourceCache.h/.cpp`
  - `ImageBrowserContextMenu.cpp`

## 10. 최종 판단

포팅은 단순한 prototype 수준을 넘어섰다. 다음 기반은 이미 사용할 수 있다.

- mixed NIF/image pane composition
- 공용 ImageCore/FD2D/Floar dependency
- 기본 image interaction과 compare layout
- thumbnail navigation
- BCn GPU fast path와 SRV cache

하지만 현재 가장 큰 위험은 “기능 몇 개가 덜 포팅됨”이 아니라 동일한 async/upload
bridge를 다시 구현하면서 FICTure2의 correctness guard가 누락된 점이다. 먼저 P0/P1
문제를 수정하고 공용 presentation 계층을 추출해야 이후 기능 parity 작업이 두
프로젝트에서 다시 갈라지지 않는다.

`ComparePane`/`PaneContent`, mixed split coordinator, Bethesda 전용
`TextureRepository` 분리는 유지하고, `ImagePane`의 binding/upload 로직과
`ThumbnailStrip`의 provider 책임을 정리하는 방향이 가장 적절하다.
