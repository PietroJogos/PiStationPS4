# PiStation PS4 — Fase 2

Data: 30/10/2026. Branch: `feature/dashboard-900-phase2`, baseada em
`feature/dashboard-900` no commit `5d852835`.

**Resultado:** diagnóstico estático, analisador de dependências e VFS experimental
implementados e testáveis. A compilação integral do emulador continua bloqueada
por rede; a Fase 2 não representa suporte funcional ao dashboard ou firmware 9.00.

## Estado inicial confirmado

O relatório [PHASE_01.md](PHASE_01.md), o código e os commits foram lidos antes
das alterações. A Fase 1 não estava concluída: o Firmware900Manager independente
existia e seus 35 testes passaram novamente, mas não havia executável do shadPS4.
O PR anterior é <https://github.com/PietroJogos/PiStationPS4/pull/1> e não foi
mesclado ou alterado por esta tarefa.

A revisão oficial preservada é `860d73b7e99559106a3ffa3d3d69f0f9237e115e`.
Não houve recriação do projeto nem alteração de main. O repositório do usuário
preserva o histórico oficial, embora não tenha sido criado como fork no GitHub.
O conector confirmou a permissão de escrita da conta conectada no próprio
`PietroJogos/PiStationPS4`; o remote upstream continua com push desativado.

A política de rede observada estava aplicada, sem `android.googlesource.com`.
A tentativa recursiva foi repetida e retornou HTTP 403 para o fdk-aac original,
commit `ee76460efbdb147e26d804c798949c23f174460b`. Não houve troca de URL ou revisão,
remoção do codec ou uso de mirror. Os demais submódulos também foram preparados
no checkout principal, nas revisões originais.

O `AGENTS.md` local exige um teste de permissão pela CLI. Esse comando falhou
porque `api.github.com` estava bloqueado; o conector confirmou `push: true`.
O marcador local exigido pelo AGENTS para falhas da CLI foi criado e excluído do
versionamento. Ele não descreve o resultado dos testes efetivamente executados.
Os testes sintéticos foram incluídos conforme a instrução explícita desta fase.

O usuário sugeriu o navegador do Codex. A abertura do PR foi solicitada no painel,
mas não havia ferramentas de leitura/controle de navegador nesta sessão. Nenhuma
configuração de rede foi alterada pela interface nem declarada como aplicada.

## Compilação e bloqueio remanescente

Sistema: Debian 13 x86-64, Clang 23.1.5, GCC 14.2, CMake 3.31.6 e Ninja 1.12.1.
As ferramentas e dependências locais da Fase 1 foram reutilizadas. Não é necessário
firmware proprietário para compilar os componentes de diagnóstico.

O CMake Debug foi executado antes das alterações na cópia baseline e novamente
depois no **checkout principal atual**, com seus submódulos preparados. Em ambos,
a geração falhou por:

```text
CMake Error at externals/aacdec/CMakeLists.txt:131 (add_library):
  Cannot find source file: fdk-aac/libAACdec/src/FDK_delay.cpp
  No SOURCES given to target: fdk-aac
```

Logo, não foi possível vincular o emulador, executar seu `--help`, sua suíte
completa ou jogos. A disponibilidade e compatibilidade do modo normal de jogos
permanecem **não verificadas nesta sessão**. O carregador real `elf.cpp` e a ponte
para logging/HLE foram verificados sintaticamente, sem substituir uma compilação
integral ou um teste funcional do carregador.
Release foi compilado e testado para a ferramenta independente. Uma compilação
Release do núcleo também permanece pendente da mesma dependência ausente.

Para desbloquear, adicione `android.googlesource.com` à lista permitida, aplique a
configuração ao runtime e repita `git submodule update --init --recursive`.
Depois compile Debug e execute os testes originais antes de integrar execução
de firmware. Os logs locais estão em `.cloud-env/logs/phase02-*.log`.

## Arquitetura investigada

| Área real | Referência | Conclusão para esta fase |
| --- | --- | --- |
| Formatos e parser | `src/core/loader/elf.h/.cpp`, novo `elf_types.h` | As definições puras foram movidas para um cabeçalho compartilhado, preservando os tipos e valores. O scanner usa os mesmos tamanhos, tags e constantes, mas realiza leitura limitada por descritores. |
| Módulo e mapeamento | `src/core/module.cpp`, `LoadModuleToMemory`, `LoadDynamicInfo`, `LoadSymbols` | Interpretam segmentos, metadados SCE, imports/exports e TLS. Não são chamados pelo scanner nem pela VFS experimental. |
| Linker e relocations | `src/core/linker.cpp`, `LoadModule`, `Resolve`, `Relocate`, `Execute` | O núcleo aplica relocations e prepara execução; o diagnóstico apenas descreve suas tabelas e destinos. Nenhuma relocation é aplicada. |
| NIDs e identidade | `ModuleInfo` e `LibraryInfo` em `module.h`, `SymbolsResolver` | IDs codificados dos nomes SCE são associados a nomes de biblioteca/módulo. O resolver real consulta nome/NID, biblioteca e módulo; sua igualdade ignora versão e tipo. O diagnóstico registra diferenças de versão conservadoramente para revisão. |
| Memória e TLS | `src/core/memory.cpp`, `SetupMemoryRegions`, `MapMemory`; `Linker::AllocateTlsForThread` e `TlsGetAddr` | O gerenciamento existente permanece responsável por memória convidada e TCB/TLS. Só os metadados PT_TLS são inspecionados nesta fase. |
| Processos | `Core::Emulator::Run`, `Linker::Execute`, kernel/process e threads | O caminho de jogos não inicializa automaticamente o conjunto de serviços do sistema. Não foi implementada orquestração de ShellCore/ShellUI. |
| VFS existente | `src/core/file_sys/fs.cpp`, `MntPoints::Mount/Open`, `IFile`, backends | Montagens existentes têm política read_only e overlays de jogos. Nenhuma montagem foi inserida nesse caminho. A VFS do diagnóstico mantém descritores próprios e não realiza mounts do SO. |
| Logs existentes | `src/common/logging/log.h`, `Common::Log`, `Core_Linker` | A ponte `shadps4_diagnostic_bridge.h` encaminha eventos ao logger existente; não adiciona outro backend de logging. A CLI independente reutiliza a coleção/exportação de eventos da Fase 1. |

Não foi feito refactor da execução de jogos, memória, áudio, controles, Vulkan,
HLE ou serviços. A única alteração no cabeçalho do carregador é incluir as mesmas
definições agora compartilhadas; a implementação `elf.cpp` permanece intacta.

## Firmware900Manager e parser de metadados

O componente continua independente, em `tools/firmware900`. Seu parser foi
separado em `elf_metadata_parser.cpp`, e a leitura binária limitada ficou em
`firmware900_binary.h`. O relatório JSON evoluiu para schema 2.

Implementado:

- ELF64 little-endian x86-64: headers, entry point, tipo, segmentos, flags,
  intervalos de memória/arquivo, alinhamento e cobertura do entry por PT_LOAD
  executável. Uma cobertura válida não demonstra que o código possa executar.
- Reconhecimento separado do contêiner ELF/SELF e da extensão SPRX/PRX; o papel
  shared_module depende do tipo ELF e não apenas do nome do arquivo.
- PT_DYNAMIC, bibliotecas DT_NEEDED, SONAME e identificadores SCE de módulos e
  bibliotecas, incluindo versões, IDs importados e exports declarados.
- Símbolos dinâmicos por metadados SCE, DT_SYMTAB com hash SysV ou SHT_DYNSYM
  com SHT_STRTAB vinculado. Imports, exports, nomes/NIDs, binding weak, tipo e
  qualificação por biblioteca/módulo são inventariados.
- Tabelas RELA e PLT RELA padrão/SCE: contagens por tipo, limites e destinos
  dentro de PT_LOAD; referências a símbolos são limitadas quando a contagem é
  conhecida. Tipos desconhecidos são registrados, nunca aplicados.
- Presença e tamanhos de PT_TLS. Não há alocação de TLS convidado.
- SELF: somente cabeçalho público e segmentos, com bits encrypted/compressed.
  Provedores SELF protegidos são identificados como indisponíveis para a análise
  de payload. Não são descriptografados, descomprimidos ou executados.

Leituras individuais ficam limitadas a 4 MiB; tabelas a 4.096 entradas, nomes a
1.024 bytes, dependências/candidatos a 8.192 no inventário, 32 candidatos por
dependência e 16.384 metadados adicionais no total. Limites são reportados como
incompletos. ELF32, outro endianness/machine e formatos sem suporte não recebem
uma declaração de compatibilidade. GNU hash sem seção/contagem suportada não é
interpretado por adivinhação: a contagem fica indisponível.

## Firmware900DependencyAnalyzer

O analisador constrói um grafo entre arquivos com metadados válidos. Diferencia:

- `available_metadata_not_loaded`: candidato com metadados compatíveis, sem carga.
- `missing_from_inventory`: não encontrado no inventário; HLE/runtime ainda podem
  mudar essa conclusão, portanto não significa serviço comprovadamente inexistente.
- `protected_self_unavailable`, metadados incompatíveis, identidade diferente,
  múltiplos provedores e diferenças de versão que exigem revisão.
- `matched_lle_export_not_loaded`: import que encontrou um export estático no
  escopo de dependências, sem comprovação de endereço carregado, ABI ou execução.
- `unresolved_static_runtime_unknown`, qualificadores inválidos, tipo sem suporte
  e `weak_undefined`, sem inventar implementações ou stubs de sucesso.

São detectados ciclos por caminhada iterativa, com limite de 64 registros.
Símbolos não qualificados são comparados no escopo das dependências alcançáveis;
exports de arquivos não relacionados não resolvem imports por coincidência.

Há uma interface de consulta ao registro HLE e uma ponte real para
`Core::Loader::SymbolsResolver::FindSymbol`. Uma entrada com endereço não nulo
recebe `hle_registered_functionality_unknown`: ela pode apontar para um stub;
não é chamada nem classificada automaticamente como funcional.

**A CLI independente não inicializa o registro HLE do emulador.** Ela exporta
`hle_registry_checked=false`; o teste desse ramo usa um registro sintético e
verifica que presença não vira alegação de implementação. A ponte foi compilada
sintaticamente. Vinculação ao executável principal e consulta de HLE em runtime
dependem da compilação integral e permanecem pendentes.

## VFS somente leitura e interface diagnóstica

`ReadOnlyFirmwareVfs` usa RAII, `openat`, `fstatat`, `pread` e descritores presos
ao diretório permitido. A enumeração também ocorre por descritores, sem seguir
links simbólicos. Componentes `..`, NUL, caminhos de host absolutos em mapeamentos,
prefixos ambíguos e links em qualquer componente são rejeitados. Arquivos
especiais/FIFOs não são lidos. As aberturas usam O_NOFOLLOW e somente leitura.
Os descritores de arquivos são mantidos durante a leitura de metadados.
Há limites adicionais de 4.096 bytes e 64 componentes de caminho para evitar
consumo excessivo durante enumeração ou sondagens.

`/` representa a pasta explicitamente escolhida. Mapeamentos adicionais ligam um
prefixo virtual a uma subpasta relativa **dentro da mesma raiz**, e o prefixo mais
específico prevalece. Mounts duplicados e diretórios inexistentes são rejeitados.
Não há método de escrita, syscall mount, mudança no SO ou montagem automática no
modo de jogos. Esse backend seguro foi implementado/validado em POSIX/Linux;
em plataformas sem esse suporte, a operação falha explicitamente.

Exemplo:

```bash
bash scripts/cloud/firmware900-check.sh
source scripts/cloud/env.sh
mkdir -p .cloud-env/reports
.cloud-env/build/firmware900/pistation-firmware900 \
  --firmware-dir /pasta/local/fornecida/pelo/usuario \
  --report .cloud-env/reports/novo.json \
  --text-report .cloud-env/reports/novo.txt \
  --mount /system=system \
  --vfs-path /system/executavel.elf
```

A pasta `system` do exemplo precisa existir dentro da raiz selecionada. Os
relatórios devem ser novos e externos à árvore analisada; arquivos existentes
são preservados. JSON e texto não incluem o caminho absoluto da raiz do host,
payloads ou chaves. Inventários e eventos usam caminhos relativos/virtuais e
escape de controles. Os relatórios locais continuam ignorados pelo Git.

A interface existente da **ferramenta diagnóstica** foi ampliada. Não houve
inserção de UI no caminho de jogos enquanto o núcleo não pode ser compilado.
O modo chama-se `Firmware 9.00 Dashboard — Experimental`; não existe opção
`--load`, e `boot_attempted` permanece falso. Retorno zero indica análise sem
erros de leitura/validação, não compatibilidade ou inicialização bem-sucedida.

## SceShellUI e SceShellCore

Os candidatos recebem categorias próprias, formato, status, entry point quando
disponível, imports e os resultados do grafo. Um nome SceShell em texto ou num
SELF protegido não é tratado como executável carregável.

**Nenhum binário real desses componentes foi analisado nesta fase.** O anexo
anterior `9.00.zip` excedeu o limite de transferência e não foi aberto. Portanto,
nenhuma lista de dependências reais do dashboard 9.00 foi confirmada. Os grafos
de ShellUI usados nos testes são sintéticos e não representam o firmware Sony.

Ainda precisam ser verificados em dados reais e em execução futura: serviços
kernel/HLE efetivos, NIDs/ABI, relocations/TLS, IPC convidado entre processos,
usuários, montagens/permissões, serviços de sistema, GNM/VideoOut, sincronização
e ordem de boot. Não há rastreamento de syscalls em runtime porque nenhum
executável convidado foi iniciado. Imports estáticos são evidência de dependência,
não um registro de chamadas observadas.

## Logs e testes

Categorias adicionais: `[ELF_LOADER]`, `[SELF_LOADER]`, `[SCESHELLUI]`,
`[SCESHELLCORE]`, `[DEPENDENCIES]`, `[VFS]`, `[HLE]`, `[LLE]` e `[ERROR]`, além
das existentes. Eventos exportam operação, entidade relacionada e resultado
técnico; falhas não são convertidas em sucesso. `Firmware900Manager::Emit`
permite encaminhá-los a `Common::Log` pela ponte existente, sem instalar outro
backend no núcleo. A CLI usa os eventos estruturados e stdout já existentes.

| Verificação | Resultado |
| --- | --- |
| Baseline da Fase 1 | 35 testes passaram antes das mudanças |
| Ferramenta Debug com Clang 23 | Build e testes executados |
| Ferramenta Release com Clang 23 | Build e testes executados |
| Ferramenta Debug com GCC 14 + ASan/UBSan | Build e testes executados; leaks não verificados, devido à restrição já observada no sandbox |
| Suíte ampliada | 61 testes Python e um executável C++ de HLE/sink, em duas entradas CTest |
| Cabeçalhos e parser original | Verificações sintáticas reais com Clang; definições compartilhadas comparadas com a versão anterior |
| shadPS4 Debug atual | Geração CMake bloqueada pelo fdk-aac; sem executável ou suíte integral |
| Jogos e dashboard | Não executados; compatibilidade não confirmada |

Os casos adicionais cobrem entry/segmentos/TLS/RELA, símbolos padrão e SCE,
resolução estática, ausência, ciclo, provedor incompatível/protegido, versões,
exports não relacionados, weak imports, qualificação inválida, VFS somente
leitura, traversal, symlinks, diretório de montagem ausente, texto/JSON,
redação da raiz e recusa de execução. Os testes da Fase 1 permanecem presentes.
Não foram usados arquivos proprietários.

## Arquivos e próximos passos

- `src/core/loader/elf_types.h` e `elf.h`: separação dos formatos compartilhados.
- `tools/firmware900/elf_metadata_parser.*` e `firmware900_binary.h`: parser seguro.
- `firmware900_dependency_analyzer.*`: grafo, imports/exports e interface HLE.
- `read_only_firmware_vfs.*`: enumeração, mapeamento e leitura confinada.
- `firmware900_manager.*`, `main.cpp`: inventário, relatórios, eventos e CLI.
- `shadps4_diagnostic_bridge.h`: integração preparada com logger/registro existentes.
- `tools/firmware900/tests/` e CMake: casos sintéticos e teste de HLE/sink.
- `.github/workflows/pistation-firmware900.yml`: cobre a branch Fase 2 e os formatos
  compartilhados. Resultados locais não são apresentados como CI remoto aprovado.
- `docs/pistation/PHASE_02.md`: este relatório.

Primeiro, liberar o domínio e concluir build/regressão do núcleo. Depois,
analisar arquivos reais legalmente fornecidos, revisar os bloqueios do grafo,
ligar a ponte ao registro/logger real e definir um teste explícito do carregador
convidado. Somente após essas validações deve ser considerada uma tentativa
controlada de carregar processos do dashboard. Não há merge automático nesta fase.
