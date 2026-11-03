# Parser dos executáveis de sistema — correção de diagnóstico

## Evidência e alcance

O usuário relatou 1.065 arquivos, 463 falhas de destino fora de PT_LOAD,
27 de limite de relocations e 12 de símbolos; SceShellCore.elf foi localizado
em system/vsh e rejeitado. Esses números são do teste do usuário. O relatório
completo, os headers e os executáveis reais não foram fornecidos nesta tarefa.
Não se afirma que todos os 502 casos foram resolvidos ou que ShellCore já passa.

## Causa concreta corrigida

O carregador original, src/core/module.cpp (CalculateBaseSize e
LoadModuleToMemory), mapeia PT_LOAD e PT_SCE_RELRO. O scanner aceitava apenas
PT_LOAD ao localizar tabelas e validar destinos de relocations. Um destino
legítimo em RELRO era rejeitado. O parser e o plano estático de memória agora
consideram os mesmos dois tipos, com as verificações de range/alinhamento/tamanho.
PT_GNU_RELRO não vira segmento mapeável independente: ele descreve proteções.

O linker original calcula destino = base_virtual_addr + r_offset. Compara-se,
no diagnóstico sem carga, r_offset com p_vaddr e p_memsz: ambos pertencem ao
espaço virtual não realocado do módulo. Não se soma o offset de arquivo ou a
posição de PT_SCE_DYNLIBDATA ao destino. Tags DT_SCE_RELA/JMPREL localizam a
**tabela** por offset dentro de DYNLIBDATA; tags ELF padrão usam endereço virtual
file-backed. Esse tratamento permanece distinto e tem teste próprio.

R_X86_64_NONE não escreve memória e não referencia símbolo: endereço e índice
não usados não são validados como se fossem uma escrita. Para tipos conhecidos,
a largura real (1/2/4/8/16 bytes) deve caber integralmente em um único segmento
mapeável. BSS usa p_memsz, não p_filesz. Não se aplicam relocations.
Tipos sem largura implementada (incluindo casos variáveis como COPY) passam a
unsupported_metadata; não se assume uma escrita de um byte nem corrupção.
A classificação conservadora de tipos desconhecidos mudou: o teste anterior
que aceitava tipo 999 com exit 0 agora exige análise incompleta com exit 1.

## Classificação e limites

- invalid_or_unreadable: estrutura inconsistente, range comprovadamente inválido
  ou erro de leitura. Entradas RELA não múltiplas de 24 e destinos conhecidos que
  ultrapassam segmentos continuam falhando. Detalhes incluem tag, índice, tipo,
  r_offset e largura do destino para permitir comparar com os segmentos no JSON.
- unsupported_metadata: layout/encoding, índices estendidos ou tipo de relocation
  não suportados. Não é uma declaração de arquivo corrompido.
- diagnostic_limit_exceeded: análise interrompida por limite de entradas ou bytes.
  Não é uma declaração de arquivo inválido ou compatível.

O teto de 4.096 entradas/tabela e o teto de leitura individual de 4 MiB permanecem;
os limites globais existentes também permanecem. Tabelas grandes não são
carregadas nem alocadas sem limite. Ranges físicos/virtuais são verificados antes
de aceitar a hipótese de limite, quando o layout é conhecido. O tamanho de
entrada desconhecido é reportado como suporte ausente, sem divisão por zero.

Para limites/estruturas sem suporte, conservam-se cabeçalhos, dependências e
metadados já lidos, com status explícito, scan incompleto e exit 1. O analisador
de dependências e serviços só usa provedores metadata_validated: um arquivo
parcial não passa a fornecer símbolos por ter metadados preservados. O total
error_count ainda inclui diagnósticos incompletos; consultar status/result e
notes para distinguir suas causas, sem interpretar todo erro como corrupção.

## ShellCore, SPRX e ShellUI

O status e a razão permanecem associados ao caminho de ShellCore e às categorias
SCESHELLCORE/ELF_LOADER. SPRX segue identificado pelo tipo ELF, não apenas pela
extensão. Para eboot.bin/eboot.elf sob NPXS20001 ou diretório SceShellUI, há agora
SceShellUI_path_candidate_unverified; nomes contendo SceShellUI já eram candidatos.
O caminho é apenas pista de inventário, sem autenticar identidade/firmware.
Não muda boot_allowed ou boot_attempted e não habilita execução.

## Testes e compilação

Linux: compilação Debug/Clang e cinco entradas CTest passaram. Scanner: 96 casos,
três exclusivos Windows ignorados; gerador: cinco casos; três verificadores C++.
A mesma suíte passou com GCC/ASan/UBSan, detect_leaks=0 conforme restrição anterior
do sandbox. Não houve validação de leaks. clang-format e git diff --check passaram.

Foram acrescentadas 14 regressões sintéticas, incluindo RELRO file-backed/BSS,
separação entre offset de tabela e destino, rejeição de offset de arquivo como
VA, NONE, larguras/overruns, tipo desconhecido, tabelas de 4.097 entradas,
layout desconhecido, byte size inválido e pista de caminho ShellUI.
Não se usam arquivos Sony. Logs: .cloud-env/logs/parser-fix-*.log.

O código novo é C++ portátil e não modifica os backends Windows/Linux de arquivos.
Windows/MSVC não foram executados neste host. O workflow existente do diagnóstico
inclui MSVC Debug/Release para validar no PR; não se declara aprovação Windows.
O núcleo do emulador não foi alterado ou recompilado nesta tarefa: o bloqueio
anterior de fdk-aac é separado, e esta mudança é na ferramenta independente.

## Próxima verificação real

Recompilar e executar o mesmo scan com relatório novo. Comparar especialmente:
status, notes, elf.segments, elf.relocation_status e diagnostics.result de
system/vsh/SceShellCore.elf. Se restar destino inválido, precisamos apenas da
entrada diagnóstica e dos metadados correspondentes (sem payload proprietário)
para identificar tipo/endereço/segmentos. Os 27/12 limites continuam exigindo uma
futura estratégia de parsing em blocos ou configuração de orçamento validada;
não foram removidos para produzir um resultado aparentemente aprovado.

Arquivos: elf_metadata_parser.cpp/.h, firmware900_manager.cpp,
firmware900_boot_diagnostic.cpp, tests/test_scanner.py e este documento.
Base: correção Windows dc550079. Nenhuma tentativa de boot real ou compatibilidade
com o dashboard foi estabelecida.
