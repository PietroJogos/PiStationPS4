# PiStation 4.1 — Windows VFS

Base: Fase 4, commit 796b2e67. Branch: fix/dashboard-900-windows-vfs.

## Causa e alcance

O backend anterior somente abria/lia/enumerava por descritores POSIX. No Windows,
o construtor lançava `secure VFS mounts require POSIX descriptor support` antes
de qualquer arquivo ser enumerado. Isso explica o inventário vazio e a cascata
de falhas do scanner, apesar de --help funcionar.

O C4702 decorre do código comum após um throw incondicional no ramo sem POSIX.
Agora o CMake seleciona uma implementação nativa distinta no Windows. O parser
extraía binding/visibility de bytes por promoção para int, comparando com
constantes de enumeração sem sinal; a extração agora é explicitamente unsigned.
Não foi possível reproduzir ou confirmar a ausência dos warnings MSVC neste host.

O teste POSIX de escaping de nomes usa aspas/controle/barra invertida, inválidos
como nomes Windows. Ele permanece integral no Linux, com skip explícito no
Windows. Um teste portátil cobre espaços, acentos, japonês e escaping em nomes
contidos nos metadados ELF. Leitura de subprocessos e relatórios Python usa UTF-8.

## Backend e segurança

- CreateFileW abre a raiz autorizada; somente discos locais fixos são aceitos.
  UNC, raízes de rede/dispositivo e unidades não fixas são rejeitadas.
- FILE_FLAG_OPEN_REPARSE_POINT e FILE_FLAG_BACKUP_SEMANTICS na raiz;
  GetFileInformationByHandleEx verifica tipo, atributos, tamanho e estado.
- Para filhos, NtCreateFile usa RootDirectory e um único componente validado,
  com FILE_OPEN, OPEN_REPARSE_POINT, OBJ_DONT_REPARSE e tipo explícito. Não basta
  comparar prefixos textuais: a resolução é relativa ao handle do pai.
- CreateFileW não fornece abertura relativa a handle; por isso NtCreateFile é
  obtido de ntdll. Se indisponível, a operação falha, sem fallback por caminho.
- Todos os reparse points são rejeitados nas aberturas e ignorados na enumeração,
  inclusive junctions e symlinks que apontariam para dentro da raiz.
- Handles RAII são movíveis. Cada arquivo conserva a cadeia de ancestrais aberta,
  inclusive uma duplicata da raiz. Compartilhamento permite apenas leitura:
  escrita, exclusão e renomeação concorrentes são impedidas ou a abertura falha.
- ReadFile usa offsets explícitos e os limites de metadados existentes: 4 MiB por
  leitura, limites de arquivo, offsets e leituras curtas/erros verificados.
- Enumeração por FileIdBothDirectoryInfo, com validação de limites dos registros,
  max_entries e profundidade. Buffers ficam no heap para evitar exceder a pilha
  Windows em árvores profundas. O manager conserva sua ordenação dos relatórios.
- Traversal, NUL, ADS (dois-pontos), prefixos de namespace, controles, caracteres
  inválidos e finais ambíguos de ponto/espaço são rejeitados. Montagens são
  revalidadas por handles a cada abertura. Nenhuma escrita é oferecida.

A raiz efetiva é o diretório aberto a partir da seleção explícita do usuário;
o scanner já canonicaliza essa seleção. Não se comprova a origem de conteúdo
ou hardlinks: um arquivo regular referenciado dentro da raiz é autorizado.
Não há snapshot de inventário nem garantia de conteúdo imutável no backend POSIX.
No Windows, conflitos de compartilhamento, acesso negado, filesystem sem suporte
às operações por handles e registros indisponíveis fazem a análise falhar de
forma explícita. Não se relaxam ACLs, privilégios ou políticas do sistema.

## Unicode e PowerShell

MSVC compila as fontes com /utf-8. wmain converte argumentos UTF-16 para UTF-8;
os caminhos de host/guest são convertidos explicitamente, sem depender da ACP.
O console conectado recebe CP_UTF8 apenas durante o comando; a página anterior
é restaurada depois de descarregar os streams. Não se muda o perfil PowerShell
ou a configuração do sistema. Relatórios gravados diretamente continuam UTF-8.
PowerShell antigo pode recodificar pipelines/redirecionamentos; use os arquivos
--report/--text-report diretamente ou PowerShell 7 para preservar o encoding.

## Arquivos modificados

- read_only_firmware_vfs_win32.cpp: backend por handles.
- firmware_vfs_paths.h: validação comum extraída e regras Windows.
- read_only_firmware_vfs.cpp/.h: seleção de handle, RAII e validação compartilhada.
- main.cpp: argumentos/console Unicode; opções de boot preservadas.
- elf_metadata_parser.cpp: extração unsigned de binding/visibility.
- CMakeLists.txt: backend por plataforma, /utf-8 e teste nativo Windows.
- tests/test_scanner.py e tests/test_win32_vfs.cpp: cobertura portátil e Windows.
- Workflow de diagnóstico: job MSVC Debug/Release.

Todos os caminhos acima ficam em tools/firmware900, exceto o workflow.
Não há alteração no núcleo do shadPS4, linker, memória, Vulkan, áudio, controles,
HLE/LLE ou processos. O problema fdk-aac fica fora desta correção.

## Validação

Neste ambiente Linux, o diagnóstico Debug e as cinco entradas CTest passaram.
A suíte relacionada também passou com GCC/ASan/UBSan (detect_leaks=0, conforme
restrição anterior do sandbox; sem validação de leaks).
Logs locais: .cloud-env/logs/windows-fix-*.log.
Os testes existentes de ELF/SELF, truncamentos, dependências, símbolos,
relocations, VFS e boot continuam presentes.

Testes Windows preparados: namespace/ADS/traversal, Unicode, junction para fora
da raiz, symlink (skip somente se privilégio 1314 indisponível), leitura por offset,
limites, bloqueio de escrita/renomeação e estabilidade da contagem de handles
após 32 ciclos de construção/destruição. A suíte Linux mantém seus testes de
symlinks, FIFO, bytes inválidos e nomes de arquivos não representáveis no Windows.

**Windows 10/11 e MSVC não foram executados neste host.** O job windows-latest
foi preparado para executar os testes de fato; sua aprovação deve ser verificada
no PR. Não se declara a correção Windows concluída antes dessa validação.
Nenhum firmware real foi analisado ou versionado.

## Validação local Windows

No PowerShell com Visual Studio C++ e Python disponíveis:

```powershell
cmake -S .\tools\firmware900 -B .\build-firmware900 -DPISTATION_ENABLE_TESTS=ON
cmake --build .\build-firmware900 --config Debug --parallel 4
ctest --test-dir .\build-firmware900 -C Debug --output-on-failure
.\build-firmware900\Debug\pistation-firmware900.exe --help
```

O teste nativo opcional de SymbolsResolver continua restrito ao Linux; não ative
PISTATION_NATIVE_REGISTRY_TESTS no Windows. Os demais alvos não exigem submódulos.
Para verificar Release, repita build/ctest com --config Release e -C Release.

--boot-diagnostic e --attempt-boot permanecem com a semântica anterior:
boot_backend_unavailable / prepare_native_execution_backend e boot_attempted=false.
Corrigir a VFS não habilita execução de firmware nem inicialização do dashboard.
