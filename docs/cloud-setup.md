# PiStation PS4 — configuração do ambiente, Etapa 1

Data: 30/10/2026. Trabalho limitado à preparação; Etapas 2–6 não foram iniciadas.

## Repositório e referência

- Repositório conectado: <https://github.com/PietroJogos/PiStationPS4>.
- Estado inicial: branch local `work`, commit `d045e08`, somente `README.md`.
- A página pública do GitHub informa `isFork: false`.
- Branch local desta preparação: `chore/cloud-environment-setup`.
- Código usado para validação: <https://github.com/shadps4-emu/shadPS4>.
- Revisão fixada: `860d73b7e99559106a3ffa3d3d69f0f9237e115e` (0.21.1 WIP).
- Cópia de trabalho separada: `.cloud-env/source/shadPS4`, ignorada pelo Git.
- Nenhum push, importação de histórico ou alteração no repositório oficial.

O PiStationPS4 conectado ainda não é um fork com o código do emulador. Para a
próxima tarefa, crie um fork usando **Fork** no repositório oficial, escolha sua
conta e conecte esse repositório na configuração do ambiente. O nome PiStationPS4
já está ocupado por este repositório de preparação; um fork com outro nome evita
sobrescrever ou excluir trabalho. Estes scripts podem depois ser transferidos
para o fork por uma branch própria. Não é necessário alterar a branch principal.

## Documentação oficial consultada

Os arquivos foram lidos na revisão acima antes de configurar o CMake:

- [README](https://github.com/shadps4-emu/shadPS4/blob/860d73b7e99559106a3ffa3d3d69f0f9237e115e/README.md).
- [Compilação Linux](https://github.com/shadps4-emu/shadPS4/blob/860d73b7e99559106a3ffa3d3d69f0f9237e115e/documents/building-linux.md).
- [CMakeLists.txt](https://github.com/shadps4-emu/shadPS4/blob/860d73b7e99559106a3ffa3d3d69f0f9237e115e/CMakeLists.txt),
  `.gitmodules`, `externals/CMakeLists.txt`, `tests/CMakeLists.txt` e o workflow oficial.
- [CONTRIBUTING](https://github.com/shadps4-emu/shadPS4/blob/860d73b7e99559106a3ffa3d3d69f0f9237e115e/CONTRIBUTING.md)
  e `LICENSE`. Futuras contribuições ao upstream têm regras próprias de revisão
  humana e divulgação de uso de IA. Esta preparação foi produzida com Codex.

A skill solicitada `cloud-environment-onboarding:setup` não estava disponível no
catálogo nem no workspace. Foi usada a skill disponível
`cloud-environment:cloud-environment-runtime` para consultar prontidão, política
de rede e instruções do proxy. Não houve publicação da configuração do ambiente.

## Sistema e ferramentas

Debian GNU/Linux 13 (trixie), x86-64. O limite do contêiner é 8 GiB de RAM,
2 CPUs de quota e nenhum swap. O script usa dois processos de compilação;
`PISTATION_JOBS=1` reduz a pressão sobre memória.

| Ferramenta | Versão verificada |
| --- | --- |
| Clang / clang-format / LLD | 23.1.5, pacote snapshot oficial do LLVM |
| GCC / G++ | 14.2.0 |
| CMake | 3.31.6 |
| Ninja | 1.12.1 |
| GNU binutils | 2.44 |
| GDB | 16.3 |
| pkg-config | 1.8.1 |
| Vulkan loader / headers do sistema | 1.4.309 |

O upstream recomenda Clang 23 e também testa GCC 14. O projeto exige CMake
3.24 ou superior e C++23; usa SDL3 e outras dependências incorporadas pelos seus
submódulos. As dependências de desenvolvimento do SO incluem áudio, OpenSSL,
zlib, libedit, udev, evdev, SDL2 conforme a documentação, Vulkan, PNG, X11,
Wayland e UUID. O instalador contém a lista completa e resolve dependências
transitivas com APT.

O usuário `agent` não dispõe de `sudo`, e `/usr`, `/etc` e seu diretório pessoal
são protegidos contra escrita. Por isso, os pacotes são baixados e extraídos em
`.cloud-env/sysroot`, sem executar scripts de manutenção nem alterar a base.
São usados repositórios oficiais Debian e LLVM, com assinaturas verificadas.
A chave pública do LLVM é conferida pelo fingerprint
`6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`.

`env.sh` ativa as ferramentas locais, pkg-config e um diretório de bibliotecas
de execução. As bibliotecas de execução da glibc do host são preservadas nesse
diretório; a compilação usa o sysroot local e a libstdc++ do GCC 14. O marcador
`ready` só é criado após o teste C++23. A instalação ocupa aproximadamente
1,4 GB, mais arquivos baixados, fontes e diretórios de compilação.

O script pode ser repetido usando seu cache; versões de pacotes podem evoluir
nos repositórios. Os SHA-256 dos arquivos realmente baixados são registrados em
`.cloud-env/logs/packages.sha256`. A revisão do código e os submódulos são fixos.
As versões dos 347 pacotes extraídos nesta sessão constam de
`.cloud-env/logs/packages.tsv`.

## Reutilização

Execute na raiz deste repositório, com acesso à rede permitido pelo ambiente:

```bash
bash scripts/cloud/setup.sh
bash scripts/cloud/prepare-source.sh
bash scripts/cloud/build.sh
```

Para uma sessão interativa ou uma verificação sem reinstalar:

```bash
source scripts/cloud/env.sh
bash scripts/cloud/check.sh
shellcheck -x -P scripts/cloud scripts/cloud/*.sh
```

O instalador suporta Debian 13 x86-64. Não use este sysroot em outra distribuição.
`PISTATION_ENV_DIR` pode mudar o diretório de instalação **antes** do setup;
depois de mover uma instalação, reconfigure o CMake, que guarda caminhos absolutos.
`PISTATION_SOURCE_DIR` e `PISTATION_BUILD_DIR` selecionam diretórios alternativos.
Os scripts usam Bash; execute-os com `bash`.

`prepare-source.sh` baixa apenas fontes abertas, valida o commit, inicializa
submódulos e verifica revisões e alterações. `build.sh` recusa submódulos
ausentes/divergentes e fontes modificadas, configura Release com Ninja e as
bibliotecas incorporadas do upstream, compila, verifica dependências dinâmicas
e executa somente `--help`. Não inicia conteúdo PS4.

Logs completos e artefatos ficam em `.cloud-env/logs` e `.cloud-env/build`.
O diretório `.cloud-env` é ignorado pelo Git; seu conteúdo não é enviado ao GitHub
por estes scripts. Configure o comando de instalação do ambiente publicado como
`bash /workspace/PiStationPS4/scripts/cloud/setup.sh` somente depois de garantir
que esses arquivos estarão disponíveis no checkout das próximas tarefas.

## Estrutura inspecionada

| Área | Local no shadPS4 |
| --- | --- |
| Executável e inicialização | `src/main.cpp`, `src/emulator.cpp`, `src/core/` |
| ELF e símbolos | `src/core/loader/`, `src/core/linker.cpp` |
| Memória | `src/core/memory.cpp` e `.h` |
| Bibliotecas HLE e kernel | `src/core/libraries/` |
| Vulkan e shaders | `src/video_core/`, `src/shader_recompiler/` |
| Controle e teclado | `src/input/` |
| Dependências | `externals/`, `.gitmodules` |
| Testes existentes | `tests/`, opção CMake `ENABLE_TESTS` |

Há 45 submódulos diretos e 52 contando os aninhados na revisão examinada.
As revisões vêm do upstream, sem atualização independente das dependências.
Essa inspeção é um inventário; a investigação de boot do sistema fica na Etapa 2.

## Estado da validação

As ferramentas foram instaladas. O teste C++23 com `std::expected`, `std::format`
e `-march=x86-64-v3` compilou e executou. As consultas de versões pelo pkg-config
para ALSA, PulseAudio, udev, evdev, X11, Wayland e Vulkan passaram. Bash e
ShellCheck foram usados para verificar os scripts. Uma nova execução completa
do instalador reutilizou o cache dos 347 pacotes e terminou com sucesso.

A compilação integral continua pendente: o submódulo obrigatório
`externals/aacdec/fdk-aac`, commit `ee76460efbdb147e26d804c798949c23f174460b`,
depende de <https://android.googlesource.com/platform/external/aac>.
A política inicial de rede rejeitou a conexão com HTTP 403. Os outros
51 submódulos foram preparados nas revisões exigidas. Nenhum espelho alternativo,
stub, remoção de codec ou modificação do emulador foi usado para contornar isso.

A tentativa real com CMake, Clang 23 e Ninja concluiu as verificações de
configuração, mas falhou na geração dos arquivos de compilação em
`externals/aacdec/CMakeLists.txt:131`: `Cannot find source file:
fdk-aac/libAACdec/src/FDK_delay.cpp` e `No SOURCES given to target: fdk-aac`.
O log é `.cloud-env/logs/configure.log`. Não há executável final; a compilação
completa, `--help` e a suíte de testes do emulador permanecem pendentes.
O preflight de `build.sh` também foi executado e recusou corretamente essa
árvore incompleta, antes de iniciar o Ninja.

O diagnóstico padrão de Vulkan não encontrou drivers no contêiner. Com o ICD
local explicitamente selecionado, `vulkaninfo --summary` conseguiu criar uma
instância e enumerar **llvmpipe**, dispositivo de CPU: Mesa 25.0.7, Vulkan
1.4.305. Isso valida somente o carregamento do driver de software; não comprova
disponibilidade de GPU física, superfície gráfica, execução de jogos ou dashboard.
Para repetir esse diagnóstico isolado:

```bash
source scripts/cloud/env.sh
mkdir -p "$PISTATION_ENV_DIR/state/runtime"
chmod 700 "$PISTATION_ENV_DIR/state/runtime"
XDG_RUNTIME_DIR="$PISTATION_ENV_DIR/state/runtime" \
VK_DRIVER_FILES="$PISTATION_PREFIX/usr/share/vulkan/icd.d/lvp_icd.json" \
vulkaninfo --summary
```

## Erros encontrados e tratamento

| Ocorrência | Causa observada | Tratamento / estado |
| --- | --- | --- |
| Primeira consulta HTTP sem conexão ao proxy | Comando executado sem acesso de rede no sandbox | Comandos de rede executados com a capacidade apropriada; downloads oficiais funcionaram |
| CMake, Ninja e Clang ausentes | Imagem inicial sem ferramentas de compilação | Instalados localmente e verificados |
| GPG tentou criar `~/.gnupg` | Diretório pessoal protegido | Diretório GPG local exclusivo para a chave pública do LLVM |
| Linker não encontrou `libm` no sysroot | Links de usrmerge não são criados por extração de pacotes | Links locais `lib`, `lib64`, `bin` e `sbin` preparados; teste C++23 passou |
| Primeira clonagem recursiva abortou | Domínio de fdk-aac bloqueado | Dependências permitidas preparadas separadamente; liberação de domínio necessária |
| Alguns submódulos ficaram sem checkout após o aborto | Índices não criados na clonagem parcial | Checkout forçado somente na cópia nova de validação; nenhuma alteração de usuário descartada |
| Primeiro configure reclamou do target zlib | Checkout incompleto de zlib-ng naquela tentativa | Fontes originais restauradas e configuração refeita com cache novo |
| Vulkan sem drivers na descoberta padrão | ICD local fora dos diretórios padrão e ausência de driver utilizável no host | Diagnóstico separado com llvmpipe local passou |
| API GitHub respondeu `Forbidden` | Acesso a `api.github.com` não disponível na política inicial | Permissão de escrita não verificada; nenhum push tentado |

As ausências detectadas em verificações opcionais do CMake não significam,
isoladamente, uma falha de compilação. Os logs distinguem essas verificações
dos erros fatais. A advertência APT sobre um arquivo de configuração protegido
não impediu o download com validação de assinaturas.

## Publicação e próxima tarefa

Arquivos preparados nesta branch, fora das fontes oficiais:

| Arquivo | Finalidade |
| --- | --- |
| `.gitignore` | Excluir sysroot, fontes de validação, caches e builds do versionamento |
| `LICENSE` | Cópia da licença GPLv2 do upstream |
| `README.md` | Estado real do projeto, créditos e acesso ao relatório |
| `docs/cloud-setup.md` | Este relatório e instruções reutilizáveis |
| `scripts/cloud/upstream.env` | URL e revisão oficial fixada |
| `scripts/cloud/env.sh` | Ativação das ferramentas locais |
| `scripts/cloud/setup.sh` | Download autenticado, extração e validação das dependências |
| `scripts/cloud/prepare-source.sh` | Preparação da revisão e submódulos originais |
| `scripts/cloud/check.sh` | Verificação das ferramentas, bibliotecas e C++23 |
| `scripts/cloud/build.sh` | Preflight, configuração, compilação e smoke test da CLI |

São recursos de preparação do ambiente. Nenhum componente C++ do shadPS4 foi
modificado. Os arquivos permanecem locais para revisão; nenhum PR foi criado.

Para retomar a validação completa, libere `android.googlesource.com` na lista de
domínios permitidos do ambiente e execute novamente `prepare-source.sh` e
`build.sh`. Mantenha os destinos do preset de gerenciadores de pacotes: Debian,
LLVM, GitHub, raw.githubusercontent.com, codeload.github.com e downloads de
releases. Para uso futuro da API GitHub, também permita `api.github.com`.

A leitura pública do repositório e `git ls-remote` funcionaram. Isso não demonstra
permissão de escrita. Para publicar branches e PRs no fork, selecione o fork na
conexão GitHub do ambiente e conceda acesso de escrita ao conteúdo e a pull
requests. Não coloque tokens nos scripts nem no repositório. Uma falha da API
com a rede bloqueada não permite concluir que a credencial seja inválida.

Nenhum firmware, chave, BIOS ou dado pessoal foi instalado ou versionado.
Nenhum módulo de firmware, suporte ao dashboard, teste com jogos ou compatibilidade
com firmware 9.00 foi implementado ou declarado. A licença original do shadPS4
foi preservada na cópia de fontes; uma cópia de GPLv2 também acompanha os scripts.

Revise a configuração e este relatório antes de publicar o ambiente. Depois,
abra uma nova tarefa no fork real para criar `feature/dashboard-900` e iniciar
a investigação de dependências e bloqueios reais do sistema PS4.
