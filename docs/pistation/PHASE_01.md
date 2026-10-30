# PiStation PS4 — Fase 1

Data: 30/10/2026. **Implementação diagnóstica disponível; Fase 1 ainda incompleta**
porque a compilação integral do emulador depende de um submódulo bloqueado pela
rede. O dashboard original não foi inicializado ou testado.

## Repositório e histórico

O repositório conectado é `PietroJogos/PiStationPS4`. Ele começou com somente um
README e o commit `d045e08`; a página do GitHub informou `isFork: false`.
O conector GitHub confirmou o usuário autenticado `PietroJogos` e permissões
`push` e `admin` nesse repositório. Isso confirma a conta e o destino autorizado,
mas não transforma o repositório em um fork na rede de forks do GitHub.

O trabalho está em `feature/dashboard-900`. Foi feita uma mesclagem de históricos,
preservando o commit inicial do usuário e os **4.572 commits ancestrais** do
upstream até `860d73b7e99559106a3ffa3d3d69f0f9237e115e` (0.21.1 WIP).
O commit da mesclagem é `0e92951f`. A licença GPL, todos os cabeçalhos originais,
os créditos e o README original foram preservados; o README recebeu uma nota
introdutória sobre o estado experimental do PiStation.

`origin` aponta para o repositório do usuário. `upstream` serve para leitura;
sua URL de push local é `DISABLED`. A branch principal não foi modificada.
Qualquer PR desta fase deve ter como destino **PietroJogos/PiStationPS4:main**,
sem publicação no repositório oficial. Esta implementação e documentação foram
produzidas com Codex e devem passar por revisão humana antes da integração.

## Ambiente e dependências

Debian 13 x86-64; limite de 8 GiB de memória e quota de duas CPUs. Clang 23.1.5,
GCC 14.2, CMake 3.31.6, Ninja 1.12.1, GDB, binutils e 347 pacotes foram extraídos
em `.cloud-env/sysroot`, devido à ausência de sudo e à base protegida contra
escrita. Os repositórios Debian/LLVM usam assinaturas verificadas.
O teste C++23 e as consultas de bibliotecas do ambiente passaram.
Detalhes e inventário: [relatório anterior](../cloud-setup.md).

As instruções oficiais de Linux, CMake, `.gitmodules`, `CONTRIBUTING.md`,
`externals/CMakeLists.txt`, `tests/CMakeLists.txt` e workflows foram consultados.
O compilador recomendado é Clang 23; GCC 14 também é testado pelo upstream.
O núcleo exige CMake >= 3.24, C++23, SDL3, Vulkan e os submódulos incorporados,
além das bibliotecas de áudio, entrada, janelas e desenvolvimento do SO.

## FDK-AAC e resultado da compilação original

Origem original mantida em `.gitmodules`:

```text
externals/aacdec/fdk-aac
https://android.googlesource.com/platform/external/aac
ee76460efbdb147e26d804c798949c23f174460b
```

A inicialização recursiva foi repetida nesta fase e falhou com:

```text
CONNECT tunnel failed, response 403
Failed to clone 'externals/aacdec/fdk-aac' a second time, aborting
```

A política observada estava `enforced`, com o preset de gerenciadores de pacotes,
sem `android.googlesource.com` na lista permitida. É necessário adicionar esse
domínio na configuração do ambiente e aplicar a configuração ao runtime antes
de repetir o comando. Não foi usado outro mirror, alterada a revisão, removido
o codec ou desativada uma funcionalidade para contornar a restrição.

A cópia original, em `.cloud-env/source/shadPS4`, contém os outros 51 submódulos
diretos/aninhados nas revisões esperadas. Seu código não foi modificado.
A configuração **Debug**, com Clang 23 e Ninja, foi executada para o núcleo.
O CMake concluiu as verificações, mas falhou ao gerar a compilação:

```text
CMake Error at externals/aacdec/CMakeLists.txt:131 (add_library):
  Cannot find source file: fdk-aac/libAACdec/src/FDK_delay.cpp
  No SOURCES given to target: fdk-aac
```

Não há executável final do shadPS4 e seu `--help` não pôde ser executado.
As configurações da suíte existente (`ENABLE_TESTS=ON`, Debug) foram tentadas
antes e depois das alterações e falharam pelo mesmo motivo. **Os testes do
emulador não foram executados**; não há declaração de regressão validada com jogos.

Evidências locais, ignoradas pelo Git:

- `.cloud-env/logs/phase01-submodules.log`.
- `.cloud-env/logs/upstream-debug-build-configure.log`.
- `.cloud-env/logs/upstream-tests-before.log` e `upstream-tests-after.log`.
- `.cloud-env/logs/upstream-results.json`.

Depois de liberar o domínio:

```bash
bash scripts/cloud/prepare-source.sh
bash scripts/cloud/build.sh
```

`build.sh` usa Debug por padrão, dois processos, fontes oficiais fixadas e
verificação de submódulos. `PISTATION_BUILD_TYPE=Release` seleciona Release.
Ele verifica as dependências dinâmicas e executa `shadps4 --help` somente depois
de compilar com sucesso. Para testar o checkout principal em tarefas futuras,
inicialize também seus submódulos; a cópia baseline é uma validação independente.

## Arquitetura inspecionada e pontos de reutilização

As referências abaixo correspondem ao código real da revisão oficial preservada.

| Responsabilidade | Arquivos e funções observados | Reutilização e limites |
| --- | --- | --- |
| Entrada e inicialização | `src/main.cpp`, `Core::Emulator::Run` em `src/emulator.cpp` | A CLI seleciona o conteúdo; Run prepara configurações, montagens, memória, entrada, HLE e linker antes de `linker->Execute(args)`. O modo diagnóstico não chama essa sequência. |
| ELF/SELF | `src/core/loader/elf.h/.cpp`, `Elf::Open`, `IsSelfFile`, `IsElfFile` | Leitura de cabeçalhos/segmentos e reconhecimento do formato existentes podem ser reaproveitados quando houver um plano de carregamento. Reconhecer SELF não garante payload utilizável. |
| Mapeamento e módulos | `src/core/module.cpp`, `Module::LoadDynamicInfo`, `LoadSymbols` | O núcleo interpreta segmentos e tabelas dinâmicas SCE, módulos importados e bibliotecas. O scanner independente lê somente metadados, com limites próprios. |
| Linker e símbolos | `src/core/linker.cpp`, `LoadModule`, `LoadAndStartModule`, `Resolve`, `Execute`; `src/core/loader/symbols_resolver.cpp` | Resolve nomes/NIDs, biblioteca, versão e módulo contra HLE e exports carregados, além de aplicar relocations e preparar TLS. Um nome de arquivo não substitui essas verificações. |
| Memória | `src/core/memory.h/.cpp`, `MemoryManager::SetupMemoryRegions`, `MapMemory`, `MapFile`, `Protect`, `UnmapMemory` | Regiões diretas/flexíveis, proteções e mapeamentos já existem. É necessário investigar as expectativas específicas dos processos do sistema antes de executar algo. |
| HLE e módulos do sistema | `src/core/libraries/libs.cpp`, `Libraries::InitHLELibs` | A tabela inclui libkernel, GnmDriver, VideoOut, UserService, SystemService, Pad e outras bibliotecas. A seleção HLE/módulos disponíveis não constitui um sistema operacional completo. |
| Serviços de kernel | `src/core/libraries/kernel/process.cpp`, `threads/`, `memory.cpp`, `file_system.cpp` | Carregamento de módulos, threads, memória e arquivos têm implementações próprias. Há stubs concretos: `sceKernelSetProcessProperty`, por exemplo, registra `(STUBBED)` e retorna `ORBIS_OK`. A cobertura exigida por ShellUI/Core precisa de investigação real. |
| Arquivos virtuais | `src/core/file_sys/fs.h/.cpp`, `MntPoints::Mount`, `MntPoints::Open`, `HandleTable` e `IFile` | Reutilizar a camada de arquivos em uma futura inicialização. Montagens de jogos não comprovam as montagens, permissões e serviços esperados pelo sistema original. |
| Vulkan | `src/sdl_window.cpp`, `Vulkan::Instance` em `src/video_core/renderer_vulkan/vk_instance.cpp`, `Presenter` e `Rasterizer` | SDL e o pipeline gráfico já existem, junto de GNM/VideoOut e recompilação de shaders. Não houve mudança gráfica nesta fase. Somente llvmpipe foi enumerado no diagnóstico anterior. |
| Controle | `src/input/controller.cpp`, `controller_state.cpp`, `input_handler.cpp` | Reaproveitar a entrada existente em fases futuras; nenhum novo mapeamento ou teste DualShock 4 foi realizado. |
| Processos e ciclo de vida | `Core::Emulator::Run`, `Restart`, `Relaunch`; `src/core/libraries/kernel/process.cpp`; `src/core/ipc/ipc.h` | O caminho estudado carrega um executável convidado e módulos. O fork do host em Relaunch serve para reiniciar o emulador. A classe IPC da aplicação não comprova os serviços IPC convidados nem a orquestração de ShellCore/ShellUI. |

O principal bloqueio de compatibilidade permanece desconhecido sem analisar
metadados reais e observar execução autorizada em fases posteriores. Será
necessário determinar o grafo de módulos, NIDs/versões/ABI, serviços de kernel,
IPC entre processos, montagens, identidade/usuários, armazenamento de estado,
GNM/VideoOut, sincronização e ordem de inicialização. Nenhuma dessas dependências
foi declarada satisfeita nesta fase.

## Firmware900Manager e modo experimental

A ferramenta está em `tools/firmware900/`, namespace `PiStation`. Seu CMake é
independente e não modifica o CMake raiz nem vincula o componente ao emulador.
Ela depende apenas da biblioteca padrão C++23. Testes usam Python >= 3.9.

Recursos implementados:

- Seleção explícita de pasta pela CLI, verificação de existência e conteúdo,
  enumeração recursiva e inventário ordenado com caminhos relativos e tamanhos.
- Reconhecimento de ELF/SELF por magic; ZIP e PSF por assinatura. Extensões
  executáveis sem assinatura são identificadas como não reconhecidas.
- Candidatos a SceShellUI/SceShellCore pelo nome, inclusive quando o conteúdo
  não é um executável. O relatório deixa explícita a natureza dessa evidência.
- Inspeção de ELF64 little-endian x86-64: cabeçalho, limites de segmentos,
  PT_DYNAMIC, DT_NEEDED, DT_SCE_NEEDED_MODULE, DT_SCE_IMPORT_LIB e as tabelas
  de strings padrão/SCE, com valores SCE conferidos em `elf.h` e `module.h`.
- Cruzamento dos nomes declarados com arquivos encontrados. `filename_candidates`
  significa apenas presença por nome; `resolution` permanece `not_evaluated`.
  A ausência de um nome não prova ausência de um serviço HLE.
- Inspeção somente do cabeçalho público SELF e da tabela de segmentos;
  sinalização dos bits encrypted/compressed. Payload não é interpretado.
- Erros por arquivo não interrompem o inventário dos demais arquivos acessíveis.
  Não são seguidos links simbólicos, nem abertos FIFOs/dispositivos.
- Relatório JSON com diagnósticos; `boot_attempted=false`, versão do firmware
  não verificada e resolução de símbolos não avaliada, em todas as execuções.
- Categorias `[PISTATION]`, `[FIRMWARE900]`, `[LOADER]`, `[MODULES]`,
  `[DEPENDENCIES]` e `[ERROR]`. Mensagens são escapadas para impedir que nomes
  com quebras de linha produzam falsas linhas de log.

Limites: 10.000 entradas por padrão, ajustável até 100.000; metadados individuais
até 4 MiB; até 4.096 cabeçalhos e dependências por arquivo, 1.024 bytes por nome,
8.192 dependências no relatório, 32 candidatos por dependência e 8.192 candidatos
no total. Truncamentos globais são reportados como incompletos e retornam erro.
`scan_complete` descreve a enumeração/limites, não a validade de todos os arquivos;
consulte também `error_count` e o status por arquivo.

Use uma árvore local estável durante o scan: não há snapshot ou garantia contra
alterações concorrentes do sistema de arquivos. Nomes UTF-8 válidos são mantidos;
bytes inválidos são substituídos por U+FFFD na saída. Não são extraídos símbolos
individuais, verificadas assinaturas criptográficas ou simuladas chamadas de sistema.
SELF, arquivos comprimidos e contêineres de atualização não são desempacotados
ou descriptografados. Não há inicialização, carregamento de módulos, instalação
de firmware ou alteração dos arquivos de entrada.

## Compilar, testar e usar o diagnóstico

No ambiente preparado:

```bash
bash scripts/cloud/firmware900-check.sh
source scripts/cloud/env.sh
mkdir -p .cloud-env/reports
.cloud-env/build/firmware900/pistation-firmware900 \
  --firmware-dir /caminho/local/fornecido/pelo/usuario \
  --report .cloud-env/reports/novo-relatorio.json
```

O arquivo de saída deve ser novo, com diretório pai existente, e ficar fora
da árvore analisada. Saídas existentes são preservadas. Retornos: `0` scan sem
erros, `1` entrada inválida/erros de arquivos/scan incompleto, `2` erro de argumentos
ou exportação. Código `0` não significa firmware 9.00 confirmado ou boot possível.

Em um sistema com ferramentas instaladas normalmente:

```bash
cmake -S tools/firmware900 -B build/scanner -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/scanner --parallel 2
ctest --test-dir build/scanner --output-on-failure
build/scanner/pistation-firmware900 --help
```

O anexo `9.00.zip` não pôde ser transferido: excede o limite de 32 MiB do executor.
Não foi aberto, extraído ou analisado. O nome do anexo não comprova seu conteúdo
nem sua versão. Os testes e o exemplo executados usam somente dados sintéticos.

## Resultados verificáveis

| Verificação | Resultado |
| --- | --- |
| Scanner Debug, Clang 23 | Compilado e executado, incluindo `--help` |
| Suíte sintética | **35 testes passaram**, em uma entrada CTest que executa unittest |
| Scanner Debug, GCC 14 + ASan/UBSan | Compilado; os mesmos **35 testes passaram**, sem erro dos sanitizadores |
| LeakSanitizer | A primeira execução com detecção de leaks falhou por restrição de inspeção de processos no sandbox; a execução aprovada usou `ASAN_OPTIONS=detect_leaks=0`, mantendo AddressSanitizer e UBSan ativos. Não houve validação de leaks |
| Formatação C++ | clang-format 23, usando `src/.clang-format` |
| Scripts | Bash syntax, ShellCheck e `git diff --check` |
| Exemplo de inventário | JSON e logs gerados a partir de ELF sintético e texto, sem conteúdo Sony |
| Núcleo original Debug | Geração CMake bloqueada por fdk-aac ausente; sem executável final |
| Suíte original antes/depois | Configuração tentada; execução bloqueada pelo mesmo submódulo |
| Compatibilidade de jogos/dashboard | Não testada; nenhuma declaração de suporte |

Os testes cobrem pastas ausentes/vazias, ELF/SELF truncados, arquitetura/formato
não suportados, overflow de offsets, tabelas/strings inválidas, limites, nomes
SCE e DT_NEEDED, presença por nome sem resolução, SELF com flags de proteção,
ZIP sem extração, links/ciclos, FIFO, Unicode/bytes inválidos, escape dos logs,
proteção da saída e 100 entradas pseudoaleatórias determinísticas.
Evidências estão em `.cloud-env/logs/scanner-*.log` e nos arquivos LastTest.log
dos dois builds. Exemplo local: `.cloud-env/reports/synthetic-firmware900.json`.

Foi adicionado um workflow específico para compilar e testar a ferramenta em
Debug e com ASan/UBSan no GitHub. Seu resultado remoto deve ser verificado no PR;
os resultados acima são das execuções locais.

## Arquivos alterados e preservação

- Importação de arquivos e histórico oficiais pela mesclagem; sem mudanças em
  `src/`, `externals/`, `.gitmodules`, `tests/` ou no CMake raiz em relação ao upstream.
- `.gitignore`: mantém as regras originais e ignora ambiente, firmware local e saídas.
- `README.md`: nota experimental, conservando o README oficial completo abaixo.
- `tools/firmware900/`: biblioteca Firmware900Manager, CLI, CMake e testes sintéticos.
- `.github/workflows/pistation-firmware900.yml`: CI do diagnóstico independente.
- `scripts/cloud/firmware900-check.sh`: build/test/--help reutilizáveis.
- `scripts/cloud/build.sh`: seleção de build, Debug por padrão.
- `docs/pistation/PHASE_01.md`: este relatório; `docs/cloud-setup.md`: nota histórica.
- Scripts de preparação e licença da tarefa anterior permanecem preservados.

Nenhum firmware, chave, BIOS, executável proprietário ou relatório sobre arquivos
reais do usuário foi adicionado ao Git. O binário diagnóstico, os pacotes, fontes
de validação e todos os relatórios de execução ficam ignorados em `.cloud-env`.

## Pendências e próximos passos

1. Liberar `android.googlesource.com`, completar o submódulo original e compilar
   o shadPS4 Debug. Executar `--help` e a suíte original antes de qualquer mudança
   de carregamento ou integração ao núcleo.
2. Revisar a branch/PR no repositório do usuário. A mesclagem importa todo o
   código porque a branch principal estava praticamente vazia; o diff adicional
   contra `upstream/main` identifica o trabalho específico do PiStation.
3. Analisar uma pasta local estável fornecida pelo usuário, sem publicar conteúdo,
   e separar SELF protegidos de ELF com metadados inspecionáveis.
4. Definir o grafo real de ShellUI/Core, ABI/NIDs e serviços necessários. Planejar
   somente então o carregamento explícito e a observabilidade em runtime.

O diagnóstico inicial, logs, testes e documentação estão implementados. A
compilação do emulador e os testes de regressão completos permanecem pendentes;
os critérios de conclusão integral da Fase 1 ainda não foram atingidos.
