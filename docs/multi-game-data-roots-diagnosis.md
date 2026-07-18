# Multi-game Game Data root 진단

작성일: 2026-07-19

## 구현 상태 (2026-07-19)

진단에서 권장한 typed-root 설계를 구현했다.

- `BethesdaGame`과 `GameDataRoot`를 도입하고 Skyrim LE, Skyrim SE,
  Fallout 4 Data root를 동시에 등록한다.
- NIF source가 등록된 Data root 아래에 있으면 그 root의 게임 ID를
  우선 사용하고, 그 외에는 BS Version(83/100/130)을 사용해 loose
  파일과 BSA/BA2 archive를 해당 게임 범위에서만 탐색한다. Skyrim SE
  Data 안의 LE-format(BS83) mesh도 SSE resource를 사용한다.
- 게임 ID가 viewport, texture cache/repository, 비동기 prefetch,
  thumbnail render, texture inspector까지 전달된다.
- `[Resources] SkyrimLEData`, `SkyrimSEData`, `Fallout4Data`를 저장하며
  기존 단일 `GameData` 설정은 자동 이관한다.
- **Detect**는 설치된 지원 게임을 모두 등록하고 **Set Active**는 현재
  NIF가 속한 게임의 Data folder를 설정한다.
- 동일 상대경로가 Skyrim SE와 Fallout 4에 동시에 존재하는 충돌
  회귀 테스트를 추가했다.

## 목적

현재 `Resources > Game Data`가 하나의 게임 Data 폴더만 사용하는 구조를
여러 지원 게임의 Data 폴더를 동시에 등록하고, 표시 중인 NIF에 맞는 Base
Game Data를 자동으로 선택하는 구조로 확장할 수 있는지 검토한다.

주요 예시는 Skyrim Special Edition과 Fallout 4가 함께 설치된 환경이다.

## 결론

구현은 가능하다. 다만 Game Data 경로를 단순 문자열 벡터로 바꾸고 등록
순서대로 검색하는 방식은 안전하지 않다.

Skyrim과 Fallout은 모두 `textures/...` 형식의 Data-root 상대경로를 사용하고
일반적인 하위 namespace가 겹칠 수 있다. 따라서 모든 게임 root와 archive를
하나의 검색 목록으로 합치면 다른 게임의 동명 리소스를 먼저 찾아 잘못
표시할 가능성이 있다.

권장 방식은 각 Data root와 archive를 게임 ID에 연결하고, NIF header의
`BS Version`으로 현재 NIF의 게임을 판별한 뒤 해당 게임의 Base Data만
검색하는 것이다.

## 현재 구현에서 이미 준비된 부분

### 여러 Bethesda 게임 설치 경로 탐지

`ResourceResolver::DetectGameDataFolders()`는 Windows Registry에서 다음
게임의 설치 경로를 확인한다.

- Skyrim Special Edition
- Fallout 4
- Skyrim LE
- Fallout New Vegas
- Fallout 3
- Oblivion
- Morrowind

탐지된 설치 경로에 `Data` 또는 `Data Files`를 붙이고, 실제로 존재하는
디렉터리만 중복 없이 반환한다.

### NIF 게임 판별 정보

`NifDocument`는 header의 `BS Version`을 읽고 `bsVersion()`으로 공개한다.
현재 parser가 지원하는 값은 다음과 같다.

- `83`: Skyrim LE
- `100`: Skyrim Special Edition
- `130`: Fallout 4

따라서 지원 범위 내에서는 별도의 추측 없이 NIF가 속한 게임을 명확하게
판별할 수 있다.

### Override folder 우선순위 검색

`ResourceResolver`는 이미 여러 Override folder를 우선순위 순으로 검색한다.
따라서 복수 경로 관리, INI 목록 저장, UI count 표시에 참고할 기존 패턴이
있다.

## 현재 제약

### Resolver가 단일 Game Data만 보관

현재 API와 상태는 단일 경로를 전제로 한다.

```cpp
void SetGameData(std::wstring dataDir);
const std::wstring& GameData() const;

std::wstring m_gameData;
```

Loose file 검색과 archive scan 모두 이 경로 하나만 사용한다.

### UI와 설정도 단일 경로 전제

현재 설정은 `[Resources] GameData` 문자열 하나로 저장된다.

- 시작할 때 저장값이 없으면 탐지 결과의 첫 번째 경로만 선택한다.
- `Browse...`는 기존 경로를 새 경로 하나로 교체한다.
- `Detect`는 현재 경로와 다른 후보 하나를 선택한다.
- UI에는 `Game Data: <path>` label 하나만 있다.

즉 여러 설치 경로를 탐지할 수는 있지만 동시에 등록하거나 유지할 수 없다.

### Resource lookup에 게임 context가 없음

현재 resolver 호출은 상대경로와 NIF 디렉터리만 전달한다.

```cpp
ResourceLocation Locate(
    const std::string& relativePath,
    const std::wstring& nifDirectory = {}) const;
```

`TextureCache`와 `TextureRepository`도 NIF의 `BS Version` 또는 game ID를
전달하지 않는다. 따라서 `m_gameData`를 단순 벡터로 바꾸더라도 어느 root를
검색해야 하는지 결정할 정보가 없다.

### Archive 목록이 하나의 flat vector

현재 `m_archives`는 단일 Game Data에서 찾은 BSA/BA2를 filename 순으로
보관하고 첫 번째 entry hit를 반환한다.

여러 게임의 archive를 같은 vector에 넣으면 서로 다른 게임의 archive가
한 우선순위 공간에 섞인다. 이는 Base Game Data 자동 선택 요구와 맞지 않는다.

## 기존 제안에 대한 검증

### 정확한 부분

- 여러 게임 경로 탐지는 이미 구현되어 있다.
- UI는 탐지 결과 중 하나만 `SetGameData()`에 전달한다.
- `m_gameData` 단일 문자열이 복수 게임 등록의 직접적인 제약이다.
- Resource UI와 INI 저장 형식도 함께 변경해야 한다.

### 수정이 필요한 부분

#### “게임 간 texture namespace가 거의 겹치지 않는다”

안전한 가정이 아니다. Bethesda 게임들은 모두 `textures/...` namespace를
사용하며 `actors`, `effects`, `clutter`, `interface`, `cubemaps` 같은 일반
하위 경로를 공유한다. Loose file과 archive entry 모두 충돌할 수 있다.

#### “모든 root에서 먼저 발견된 파일을 사용하면 된다”

잘못된 게임의 리소스를 조용히 선택할 수 있다. 자동 선택 기능에서 silent
mis-resolution은 missing texture보다 진단하기 어렵고 비교 결과도 왜곡한다.

#### “Override folder와 동일한 위험이다”

Override folder는 사용자가 의도적으로 지정한 우선순위다. 서로 다른 게임의
Base Data는 override 관계가 아니므로 등록 순서가 리소스 우선순위가 되어서는
안 된다.

#### “BS Version보다 first-hit fallback이 더 견고하다”

현재 지원 게임은 `83`, `100`, `130`으로 명확하게 구분되므로 반대다.
게임을 확정할 수 있을 때는 해당 게임 root만 검색하는 것이 더 정확하다.
Fallback은 게임을 판별할 수 없는 경우에만 제한적으로 사용해야 한다.

### 지원 게임과 탐지 게임의 차이

Registry detector는 7개 게임을 찾지만 현재 NIF parser가 지원하는 게임은
Skyrim LE, Skyrim SE, Fallout 4뿐이다. 자동 등록 UI에서는 “탐지 가능한
게임”과 “현재 NIFDiff가 지원하는 게임”을 구분해야 한다.

## 권장 데이터 모델

```cpp
enum class BethesdaGame
{
    SkyrimLE,
    SkyrimSE,
    Fallout4,
    Unknown
};

struct GameDataRoot
{
    BethesdaGame game { BethesdaGame::Unknown };
    std::wstring path;
};
```

Resolver는 단순 경로 목록 대신 typed root 목록을 보관한다.

```cpp
void SetGameDataRoots(std::vector<GameDataRoot> roots);
const std::vector<GameDataRoot>& GameDataRoots() const;
```

Archive도 game/root affinity를 유지해야 한다.

```cpp
struct LoadedArchive
{
    BethesdaGame game;
    std::wstring rootPath;
    std::wstring path;
    std::unique_ptr<Floar::IArchiveReader> reader;
};
```

게임별 vector 또는 game 필드를 가진 목록을 사용할 수 있지만 lookup 시에는
반드시 요청 game과 일치하는 archive만 검색해야 한다.

## 권장 검색 순서

표시 중인 NIF의 game ID가 확인된 경우:

1. 명시적 Override folders
2. NIF parent directory
3. NIF 경로에서 유도한 mod Data root
4. 동일 game ID의 Base Game Data loose files
5. 동일 game ID의 BSA/BA2 archives

다른 게임의 Base Data는 검색하지 않는다.

`BethesdaGame::Unknown`인 경우에는 다음 중 하나를 정책으로 선택해야 한다.

- 사용자에게 active game 선택 요구
- 정확히 하나의 root에서만 발견된 경우에만 사용
- 등록 순서 fallback을 허용하되 ambiguity를 UI/log에 명시

silent first-hit fallback은 권장하지 않는다.

## Game ID 전달 경로

다음 호출 경로에 game context를 추가해야 한다.

1. `NifDocument::bsVersion()`을 `BethesdaGame`으로 변환
2. `NifViewport`가 document를 받을 때 game ID 보관
3. `TextureCache`에 NIF directory와 game ID 설정
4. `TextureRepository`의 lookup/prefetch 요청에 game ID 전달
5. `ResourceResolver::Locate/Find`가 해당 game root/archive만 검색
6. Thumbnail renderer도 parse된 `NifDocument`의 game ID를 전달
7. Texture Inspector의 직접 lookup도 active NIF의 game ID를 전달

Texture lookup memo/cache key가 상대경로와 NIF directory만 사용한다면 game
ID도 key에 포함해야 한다. 최종 `sourceKey`는 실제 loose/archive 경로를
포함하므로 source 단위 GPU dedup에는 계속 사용할 수 있다.

## UI 권장안

현재 단일 label 대신 등록된 Base Game Data를 확인하고 관리할 수 있어야 한다.

- 요약 label: `Game Data: 3 games`
- Detect: 지원 게임의 발견된 root를 모두 등록 또는 갱신
- Manage dialog/list:
  - Game
  - Data path
  - Detected/Manual 상태
  - Browse/Replace
  - Remove
- 경로가 없거나 유효하지 않은 게임을 명확히 표시
- 현재 active NIF가 사용하는 game/root를 inspector 또는 tooltip에서 확인

수동 경로 등록 시에는 경로만으로 게임을 항상 판별할 수 없으므로 game 선택을
함께 받아야 한다.

## INI 마이그레이션

현재 scalar 설정:

```ini
[Resources]
GameData=C:\...\Data
```

권장 형식은 게임별 고정 key다.

```ini
[Resources]
SkyrimLEData=C:\...\Skyrim\Data
SkyrimSEData=C:\...\Skyrim Special Edition\Data
Fallout4Data=C:\...\Fallout 4\Data
```

또는 typed list를 사용할 수 있지만 path delimiter escaping과 game label
파싱이 필요하다. 현재 지원 게임 수가 고정되어 있으므로 개별 key 방식이
단순하고 사람이 수정하기도 쉽다.

마이그레이션 시 기존 `GameData` 값은 다음 순서로 처리할 수 있다.

1. Registry 탐지 결과와 path를 비교해 game 확인
2. 경로 또는 archive 특성으로 확정 가능한 경우 해당 game key로 이동
3. 확정할 수 없으면 `Unknown` 수동 root로 보존하고 사용자에게 game 지정 요구

기존 값을 조용히 삭제하면 안 된다.

## Archive scan 고려사항

여러 Data root를 스캔하면 startup scan 비용과 archive 수가 증가한다.

- root별 scan 결과를 game별로 분리
- 서로 다른 root scan의 병렬화 여부 검토
- root 변경 시 해당 game archive만 재스캔하는 증분 방식 고려
- scan generation을 사용해 오래된 결과가 새 설정을 덮지 못하게 방지
- `ArchiveCount`도 전체 수와 game별 수를 구분

현재 filename 정렬 기반 archive 우선순위는 같은 게임 내부에서만 유지하고,
서로 다른 게임 archive 사이에는 우선순위를 만들지 않아야 한다.

## 예상 작업 범위

단순한 `wstring`에서 `vector<wstring>`으로의 변경보다 범위가 크다.

- `ResourceResolver` typed root 및 game별 archive 구조
- NIF BS Version → game ID 변환
- `NifViewport`/`TextureCache`/`TextureRepository` lookup API 변경
- thumbnail 및 Texture Inspector 호출 경로 변경
- Resource UI 관리 기능
- INI 저장과 기존 설정 마이그레이션
- cache invalidation 및 archive scan 동기화
- game collision/ambiguity 테스트

중간 이상 규모의 기능 변경으로 보는 것이 적절하다.

## 필수 테스트

1. Skyrim SE와 Fallout 4 Data가 모두 등록된 상태에서 각각의 NIF가 자기
   게임 root의 loose texture를 선택한다.
2. 두 게임 root에 동일한 상대경로가 존재해도 NIF game에 맞는 파일을 선택한다.
3. 동일 상대경로가 양쪽 archive에 존재해도 올바른 game archive를 선택한다.
4. Override folder가 동일 game의 Base Data보다 우선한다.
5. mod 폴더에서 직접 연 NIF는 derived Data root를 Base Data보다 우선한다.
6. BS Version 83/100/130 mapping이 각각 LE/SE/FO4로 동작한다.
7. `Unknown` game에서 ambiguity 정책이 silent first-hit을 만들지 않는다.
8. 기존 단일 `GameData` INI 설정이 손실 없이 마이그레이션된다.
9. root 추가/삭제 후 texture memo, GPU cache, archive scan 결과가 올바르게
   갱신된다.

## 권장 완료 조건

- 지원 게임의 Data root를 동시에 등록하고 개별 관리할 수 있다.
- NIF의 BS Version에 따라 일치하는 Base Data와 archive만 검색한다.
- 서로 다른 게임의 동일 상대경로가 잘못 선택되지 않는다.
- 기존 Override/NIF-local/derived-root 우선순위가 유지된다.
- 기존 단일 Game Data 설정 사용자의 경로가 보존된다.
- UI에서 등록 상태와 active NIF가 사용 중인 game root를 확인할 수 있다.
