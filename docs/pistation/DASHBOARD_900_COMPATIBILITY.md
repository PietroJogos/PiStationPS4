# Compatibilidade do dashboard PS4 9.00

Atualizado em 30/10/2026, Fase 3. **Dashboard não inicializado; compatibilidade
desconhecida.** Diagnósticos sintéticos não confirmam suporte ao sistema PS4.
Histórico: [Fase 1](PHASE_01.md), [Fase 2](PHASE_02.md), [Fase 3](PHASE_03.md).

Nenhum SceShellUI/Core real foi analisado. O anexo anterior excedeu o limite de
transferência. Não há lista confirmada de módulos/NIDs necessários ou rastreamento
de inicialização real. A tabela descreve o código público, não imports comprovados.

| Componente | Classificação atual | Evidência e limite |
| --- | --- | --- |
| SceShellUI / SceShellCore | Desconhecido | Sem metadados reais, carga, entry ou renderização testados. |
| UserService para ShellCore | Parcial no código | InitializeForShellCore (Psl9mfs3duM) e TerminateForShellCore são stubs em userservice.cpp; não chamadas. |
| Usuário inicial | Não verificado para dashboard | GetInitialUser (CdWp0oHWGr0) consulta/valida argumentos; uso por ShellUI/Core não testado. |
| AppMessaging / IPC | Parcial no código | SendMsgToShellCore (HIwEvx4kf6o), SendMsgToShellUI e ReceiveMsg são stubs em systemservice.cpp; protocolo desconhecido. |
| Propriedades de processo | Parcial no código | SetProcessProperty (-W4xI5aVI8w) registra stub e retorna sucesso no upstream; não alterado ou tratado como funcional. |
| Processos simultâneos | Arquitetura pendente | Linker, memória, entry_params, PID e TLS usam estado compartilhado; sem orquestração nesta fase. |
| Threads/prioridades/sincronização | Não verificado para dashboard | Código existente investigado; nenhum teste convidado nesta fase. |
| Relógio/timers/eventos | Não verificado para dashboard | ClockGettime (QBi7HCK03hw) e equeues têm código; requisitos do sistema não confirmados. |
| Sysmodule | Parcial no código | Carga/validação existem; carga por nome interno e unload contêm stubs. |
| AppContent | Parcial no código | Inicialização e algumas operações têm código; outras contêm stubs; sem teste real do sistema. |
| Memória/TLS | Não verificado para dashboard | Plano estático disponível; orçamento, parametrização e alocação não testados. |
| LLE do sistema | Não verificado | Nenhum módulo real identificado como disponível; matches sintéticos não comprovam carga/ABI. |
| GnmDriver/VideoOut/Vulkan | Não verificado para dashboard | Registros indexados; sem menus ou framebuffer reais renderizados. |
| Teclado/DualShock 4 | Não verificado para dashboard | Entrada original preservada, sem navegação testada. |
| VFS diagnóstico | Implementado/testado para diagnóstico | Montagens explícitas, leitura confinada, traversal/symlinks e erros testados; não é filesystem completo PS4. |

Nenhum serviço PS4 está classificado como **implementado e testado para o
dashboard**. Também não se comprovou ausência global de um serviço: faltam o
grafo real e o registro selecionado em runtime. `available_by_lle_metadata`
significa metadados encontrados; `absent_from_checked_providers` restringe a
ausência aos provedores consultados.

## Bloqueios e próximos critérios

1. Compilação integral: fdk-aac original bloqueado por CONNECT 403. Aplicar a
   permissão do domínio e executar build/testes originais.
2. Conteúdo real inspecionável: analisar diretório local legalmente fornecido,
   sem publicar arquivos/relatórios pessoais. SELF protegido continua sem
   payload analisável por esta ferramenta.
3. Contratos HLE/LLE: verificar imports reais, versões, NIDs e seleção de registro.
   Uma função registrada ou stub que retorna 0 não comprova comportamento.
   Preparar testes sintéticos/homebrew para contratos conhecidos.
4. Processos/IPC: definir contextos isolados, ciclo de vida, endpoints, protocolos
   e sincronização com evidência; não assumir isolamento chamando Execute duas vezes.
5. Gráficos/entrada: avaliar após carga controlada e observável, preservando os
   testes/jogos existentes. Sem promessa de boot ou prazo.

Para promover um item, registrar revisão, ambiente, método e resultado
observável. Separar diagnóstico, resolução, carga, inicialização e comportamento
funcional. Nome de arquivo, quantidade de símbolos e exit 0 não provam boot.

Relatórios JSON/texto contêm evidências e bloqueios, sempre com
`boot_attempted=false`. Não instalam, descriptografam, carregam ou executam
firmware. Os originais permanecem somente leitura; a versão 9.00 não é autenticada.
