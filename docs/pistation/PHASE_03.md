# PiStation PS4 — Fase 3

Data: 30/10/2026. Branch `feature/dashboard-900-phase3`, baseada na Fase 2,
commit `3024b1214d079017ac8e71614f887acca8a544c7`.
Implementação/testes no commit `53638b5a`. Código e documentação produzidos com
Codex, preparados para revisão humana antes de integração.

**Resultado: diagnóstico de serviços e plano estático de inicialização.** Não
há boot do dashboard, novo kernel ou orquestração de processos convidados.
A ferramenta independente é validada separadamente do emulador completo,
cuja compilação permanece bloqueada. Compatibilidade 9.00 não confirmada.

## Continuidade

Foram lidos [PHASE_01.md](PHASE_01.md), [PHASE_02.md](PHASE_02.md), os commits,
Firmware900Manager, Firmware900DependencyAnalyzer e os logs anteriores. Antes
das mudanças, passaram novamente as duas entradas CTest da Fase 2: 61 testes
Python e o teste C++ de HLE/sink. O PR #2 não foi mesclado ou modificado.

Não há metadados reais de SceShellUI/Core analisados nas fases anteriores.
O anexo `9.00.zip` excedeu o limite de transferência de 32 MiB; não foi aberto
ou extraído. Portanto, o conjunto real de imports, serviços HLE necessários e
módulos LLE disponíveis para o dashboard permanece desconhecido. Exemplos LLE
desta fase são sintéticos. Nomes de arquivos não confirmam identidade ou versão.

O conector confirmou `push: true` em `PietroJogos/PiStationPS4`. A CLI exigida
pelo AGENTS voltou a receber Forbidden; o marcador local exigido por essa regra
permanece ignorado e não é publicado. Testes foram incluídos por instrução
explícita do usuário. Main, GPL e o histórico upstream foram preservados.

## Cinco bloqueios prioritários

| Bloqueio | Impacto / dificuldade | Tratamento |
| --- | --- | --- |
| Registro de símbolo confundido com implementação | Alto / média | Firmware900ServiceManager e evidências de código, registro ativo e LLE separados. |
| Imports inválidos chegando ao callback; erros/logs acumulados na reanálise | Alto / baixa | Validação antes da consulta, isolamento de exceções e substituição de resultados, com regressões. |
| Entry/memória/TLS/parâmetros sem plano revisável | Alto / média | Boot Diagnostic estático, sem carga ou threads convidadas. |
| Build/regressão integral bloqueados pelo fdk-aac | Alto / baixa após liberação da rede | Tentativas registradas; requer aplicar a configuração que permite android.googlesource.com. |
| Grafo real e IPC/ciclo de vida simultâneo desconhecidos | Crítico / alta | Investigação e plano técnico; nenhuma ABI ou mensagem inventada. |

Os três primeiros foram tratados no alcance do diagnóstico. Não foi escolhida
uma função do kernel para implementação sem import real, contrato conhecido e
teste correspondente. Nenhum serviço existente foi alterado arbitrariamente.

## Arquitetura

`Firmware900Manager::DiagnoseServices` coordena o analisador de dependências
existente e `Firmware900ServiceManager`. O novo componente consome os resultados:
não aplica relocations, calcula endereços funcionais ou cria outro linker.
`Firmware900BootDiagnostic::Analyze` produz o plano somente quando solicitado.
Não há API de execução nesses componentes.

### Evidência do código HLE

`generate_service_catalog.py` indexa argumentos literais de LIB_FUNCTION/LIB_OBJ
nas fontes existentes de kernel, SystemService, UserService, Sysmodule,
AppContent, GnmDriver e VideoOut. O CMake regenera a saída no diretório de build
quando as fontes mudam, sem manter outra tabela manual no Git.

Revisão atual: **35 arquivos, 1.598 registros literais e 101 registros não
interpretados**. Há 939 registros associados a uma função que contém a mensagem
`(STUBBED)` no corpo; aliases podem apontar à mesma função. Isso não mede a
quantidade de funções do dashboard ou sua compatibilidade.

Cada evidência conserva NID, biblioteca, versão, módulo, tipo, função e referência
de arquivo/linha. SHA-256 identifica o conjunto de fontes. Comentários são
excluídos e os marcadores são associados ao corpo da função com PS4_SYSV_ABI.
O índice não é um parser C++ completo: não expande macros/templates ou avalia
pré-processamento, condições de registro, seleção HLE/LLE e configuração.
Ausência no índice não prova ausência de HLE. Um marcador pode existir só em um
ramo; indica revisão de implementação parcial, não falha de toda chamada.
Sem marcador, o comportamento continua não verificado.

### Registro ativo e classificação

A ponte existente consulta o SymbolsResolver original e exige endereço não
nulo, sem chamá-lo. A CLI não executa InitHLELibs: registro ativo desconectado.
O teste nativo compila **src/core/loader/symbols_resolver.cpp original**, insere
registros sintéticos e usa a ponte e Firmware900Manager. Não há substituto de
FindSymbol. No alvo isolado Linux, function/data sections e --gc-sections
descartam DebugDump e suas dependências não utilizadas. O alvo opcional precisa
dos cabeçalhos do submódulo fmt; não testa a inicialização completa das HLE.

| Classificação JSON | Significado |
| --- | --- |
| implemented_and_tested | Reservado; nenhum serviço recebe esse status nesta fase. Testar o diagnóstico não testa uma função PS4. |
| partially_implemented | Identidade/versão/tipo correspondem a registro literal com marcador de stub; seleção e comportamento em runtime não verificados. |
| available_by_lle_metadata | Módulo/export estático encontrado, sem carga, inicialização ou teste de ABI. |
| absent_from_checked_providers | Não encontrado no registro ativo fornecido e no escopo LLE estático, com inventário verificável; não é ausência global no PS4. |
| unknown_or_unverified | Cobertura insuficiente, registro sem teste funcional, erro, ambiguidade ou metadados inválidos. |

Registro ativo encontrado mantém `hle_registered_functionality_unknown` na
resolução. A igualdade nativa ignora versão/tipo, comportamento confirmado pelo
teste sem alteração do núcleo. O índice de código exige versão/tipo para anexar
evidência. Contadores distinguem conexão, consultas efetivas e consultas com erro.
`services.complete` descreve conclusão da análise dos imports observados, dentro
dos limites; não significa cobertura completa de HLE ou compatibilidade.

### Estabilidade e eventos

Imports SCE qualificados precisam ter biblioteca/módulo e NID de 11 caracteres
no alfabeto do projeto. É validação sintática, sem autenticar o identificador ou
confirmar ABI. Símbolos ELF comuns não são obrigados a ter formato NID. Tipos
sem suporte e qualificadores inválidos são rejeitados antes do callback HLE;
TLS não é convertido silenciosamente em NoType.

Exceções do callback, inclusive não derivadas de std::exception, são registradas.
Reanálise substitui seus próprios eventos/erros e invalida o plano anterior, sem
apagar erros do scanner/VFS. Falhas recuperadas não ficam acumuladas.

Os eventos de import existentes recebem solicitante, biblioteca, módulo, função
conhecida, NID, resolução, motivo e `return_code: null`, sem duplicar uma mensagem
por serviço. Identidades de bibliotecas distintas não são confundidas por NID.
Categorias específicas: KERNEL, THREAD, SYSMODULE, USERSERVICE, SYSTEMSERVICE e
IPC; BOOT/PROCESS descrevem o plano. As categorias anteriores são preservadas e
só há mensagens quando existe evidência, sem simular chamadas. A ponte mantém
Common::Log como backend; a CLI reutiliza a exportação estruturada existente.

## Investigação do kernel e serviços

| Área | Código inspecionado e conclusão |
| --- | --- |
| Processos | core/linker.cpp, kernel/process.cpp e threads/pthread.h: Execute usa módulo 0, entry_params global e uma thread principal; getpid retorna GLOBAL_PID. SetProcessProperty é stub. Não demonstra multiprocessos convidados. |
| Threads/prioridade | kernel/threads/pthread.cpp, pthread_attr.cpp e thread_state.cpp: criação, join, estados, pilhas, coleta e validação de atributos/prioridades existem; sem teste convidado nesta fase. |
| Sincronização/handles | threads/mutex.cpp, semaphore.cpp, event_flag.cpp e core/file_sys/fs.h: mutexes, semáforos, eventos e tabelas/slots existentes; contratos específicos do dashboard desconhecidos. |
| Relógio/eventos | kernel/time.cpp e equeue.cpp: clocks, sleep, equeues e timers; registro não comprova adequação ao ShellCore. |
| Memória | core/memory.cpp/.h e kernel/memory.cpp: MemoryManager singleton fornece regiões, mapas e proteções. O diagnóstico não o inicializa. |
| TLS | threads/tcb.cpp e Linker::AllocateTlsForThread/TlsGetAddr: TCB/DTV dependem do linker e módulo principal; somente tamanhos PT_TLS usados no plano. |
| Sysmodule | libraries/sysmodule/sysmodule.cpp: há carga/validação; carga por nome interno e vários caminhos de unload contêm stubs. |
| Usuários | libraries/system/userservice.cpp: GetInitialUser consulta/valida ponteiro; InitializeForShellCore e TerminateForShellCore são stubs. Sessões e persistência do dashboard não testadas. |
| SystemService/IPC | libraries/system/systemservice.cpp e core/ipc/ipc.*: AppMessagingSendMsgToShellCore/UI e ReceiveMsg são stubs. A classe IPC existente atende host/launcher; não comprova IPC convidado. |
| AppContent | libraries/app_content/app_content.cpp: inicialização, parâmetros e algumas montagens têm código; várias operações são stubs. Sem testes reais do sistema. |
| VFS | core/file_sys/fs.cpp/.h e tools/firmware900/read_only_firmware_vfs.*: montagens/backends/overlays de jogos preservados; diagnóstico usa seu backend confinado por descritores. |

Essas observações não constituem um bug funcional de kernel reproduzido em
firmware real. Nenhuma implementação existente de memória, processo, threads,
Vulkan ou HLE foi modificada nesta fase.

Montagens diagnósticas continuam configuráveis: `--mount /system=system`, quando
system existe dentro da raiz escolhida, por exemplo. Outros prefixos são
explícitos. Não há cópia automática do filesystem PS4 nem escrita nos originais.
Permissões do host, O_NOFOLLOW, proteção contra traversal/symlinks e logs de
arquivos ausentes continuam ativos. Sondagens CLI leem no máximo 16 bytes e não
expõem payload. A configuração não modifica o modo normal de jogos.

## PS4 Firmware 9.00 Boot Diagnostic

`--boot-diagnostic` inspeciona diretório, inventário, headers, dependências,
serviços, memória, bloqueios e encerra. Essa é a sequência do diagnóstico,
não uma ordem inferida de boot. Nenhum inicializador de módulo é chamado.

Cada plano contém entry, candidato a processo, PT_LOAD, soma de tamanhos, união
de intervalos arredondados a páginas PS4 de 16 KiB, PT_TLS e presença de
PT_SCE_PROCPARAM. É limite inferior de metadados, sem pilhas, heap, TCB/DTV,
reservas, buffers gráficos, base de relocação ou orçamento efetivo. Não aloca
memória. Overflow invalida a estimativa; sobreposição e write/execute exigem revisão.

Bloqueios incluem entry sem cobertura executável, procparam ausente/não validado,
argumentos/contexto/pilha não preparados, relocations não aplicadas, TLS não
alocado, SELF sem payload analisável, serviços não verificados/stubs e ciclos
que envolvem a imagem. `boot_allowed=false` e `boot_attempted=false` sempre.

O início de um ELF sintético pelo linker completo permanece bloqueado pelo build
integral. Os testes constroem/inspecionam metadados sintéticos. Threads de host
testam consultas imutáveis e diagnósticos independentes com join; não validam
criação, prioridades ou sincronização de threads PS4.

## Plano técnico para processos

1. Concluir build/regressão originais e obter inventário real legalmente fornecido.
   Confirmar entry, segmentos, imports, versões e parâmetros.
2. Validar estruturas/ponteiros de procparam dentro de segmentos e a ABI de
   argv/stack/TLS antes de reutilizar Module/Linker. Revisar inicializadores e
   ciclos, sem presumir ordem pelo nome de arquivo.
3. Testar primeiro um único executável sintético controlado, sem firmware ou I/O
   externo, com tempo limitado e encerramento observável. Esta ferramenta não
   chama Execute; fornece o plano revisável.
4. Definir contexto por processo: espaço de endereçamento, módulos/resolver,
   handles, PID, credenciais, montagens, TLS, threads, eventos e ciclo de vida.
   Os singletons atuais impedem assumir isolamento ao chamar Execute duas vezes.
5. Determinar com evidência real transporte, endpoints, mensagens, permissões e
   timeouts de ShellCore/ShellUI. Isolar um teste em processo de host não implementa
   IPC PS4. Só então decidir a arquitetura de processos simultâneos.
6. Testar falhas/finalização/limpeza, persistência separada dos originais, GNM/Vulkan
   e entrada antes de qualquer tentativa controlada de boot do dashboard.

## Compilação e testes

Ambiente: Debian 13 x86-64, Clang 23.1.5, GCC 14.2, CMake 3.31.6 e Ninja 1.12.1.

| Verificação | Resultado |
| --- | --- |
| Baseline Fase 2 | 61 testes Python + teste C++ HLE/sink passaram |
| Diagnóstico Debug/Clang 23 | Build e cinco entradas CTest passaram |
| Diagnóstico Release/Clang 23 | Build e cinco entradas CTest passaram |
| Diagnóstico Debug/GCC 14 + ASan/UBSan | Build e cinco entradas CTest passaram, sem diagnóstico dos sanitizadores |
| Formatação/scripts | clang-format 23, ShellCheck, Bash syntax e git diff --check passaram |
| Núcleo Debug, ENABLE_TESTS=ON | Geração CMake bloqueada antes e depois das alterações por fdk-aac ausente |
| Núcleo Release, ENABLE_TESTS=ON | Configuração tentada; geração bloqueada pelo mesmo arquivo ausente |
| Jogos, dashboard e kernel/threads convidados | Não executados; regressão funcional não verificada |

A suíte atual contém **77 testes Python do scanner, cinco testes Python do
gerador e três executáveis C++ de verificação**, distribuídos em cinco entradas
CTest, nas três configurações acima. São cobertos stubs conhecidos, registro
sem prova funcional, identidade e sintaxe NID, ausência limitada ao escopo,
callbacks com falha/recuperação, reanálise sem duplicação, memória/páginas/TLS,
parada segura, SELF protegido, relatórios e montagens confinadas. Os casos
anteriores de parser, dependências e VFS permanecem presentes.

ASan foi executado com `ASAN_OPTIONS=detect_leaks=0`, mantendo UBSan com
`halt_on_error=1`. LeakSanitizer permanece desativado pela restrição de inspeção
de processos observada nas fases anteriores; leaks não foram validados.
Os builds da ferramenta não emitiram warnings ou erros de linker.

O exemplo local `.cloud-env/reports/phase03-example.json` e seu relatório texto
usam exclusivamente três arquivos sintéticos: ShellCore importa o NID público
do stub de UserService; ShellUI importa um export de libSynthetic e uma biblioteca
ausente. O resultado registra um serviço parcial, metadados LLE disponíveis e
ausência do inventário com HLE desconhecido. `boot_attempted=false`, zero chamadas
HLE e zero erros de leitura/análise; não há execução de firmware nesse exemplo.

O domínio android.googlesource.com segue ausente da política aplicada. O comando
de submódulo recebeu CONNECT 403 duas vezes. CMake falhou por
`fdk-aac/libAACdec/src/FDK_delay.cpp` ausente, sem fontes para o alvo fdk-aac.
Não houve troca de URL/revisão, remoção de codec ou alteração do núcleo para
contornar a configuração. A suíte original não pôde ser executada.

Reprodução no ambiente:

```bash
PISTATION_NATIVE_REGISTRY_TESTS=ON bash scripts/cloud/firmware900-check.sh
source scripts/cloud/env.sh
.cloud-env/build/firmware900/pistation-firmware900 \
  --firmware-dir /caminho/local/fornecido/pelo/usuario \
  --boot-diagnostic \
  --report .cloud-env/reports/novo-boot.json \
  --text-report .cloud-env/reports/novo-boot.txt
```

O teste nativo exige fmt original. Sem ele, omita a variável; os demais testes
não dependem dos submódulos. Python >=3.9 agora também é dependência de build
para gerar o catálogo, mesmo com testes desativados.

JSON **schema 3** preserva campos anteriores e acrescenta services, boot_diagnostic
e eventos de serviço. Saídas precisam ser novas e externas à raiz. Caminhos do
host são omitidos. Exit 0 significa diagnóstico sem erros de leitura/análise,
mesmo com bloqueios de compatibilidade; 1 é erro/incompletude e 2 argumento ou
exportação inválidos. Nenhum código confirma boot.

Logs ignorados: `.cloud-env/logs/phase03-*.log` e LastTest.log dos builds.
O workflow foi atualizado para a branch Fase 3, fontes do catálogo e resolver.
Resultados locais não são apresentados como CI remoto aprovado.

## Arquivos e pendências

- Firmware900ServiceManager, source_service_catalog.h e gerador: evidência/classificação.
- Firmware900BootDiagnostic: plano e bloqueios por imagem.
- firmware900_service_report: JSON/texto/eventos.
- Manager, analisador, CLI e ponte: integração, validação e reanálise.
- CMake/testes, script cloud e workflow: verificações reproduzíveis.
- Este documento e [DASHBOARD_900_COMPATIBILITY.md](DASHBOARD_900_COMPATIBILITY.md).

Não há mudança em src/, CMake raiz, .gitmodules ou testes originais em relação
à Fase 2. Nenhum arquivo proprietário, executável convidado, relatório real do
usuário ou dado pessoal foi versionado. Não há merge automático.

Próximo passo: aplicar a liberação da rede, completar fdk-aac e concluir o
build/regressão integral. Depois analisar imports reais e priorizar contratos de
serviços com testes específicos. Os critérios integrais permanecem parciais.
