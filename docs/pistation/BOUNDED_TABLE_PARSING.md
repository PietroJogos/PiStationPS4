# Tabelas ELF/SCE em blocos e orçamentos separados

Base: correção de PT_SCE_RELRO, commit d91d3bfb (PR #5).
Branch: fix/dashboard-900-bounded-tables.

## Problema reproduzido

O JSON fornecido pelo usuário continha 1.065 arquivos, 27 limites de relocations,
12 de símbolos e 449 erros do orçamento global de metadados. ShellCore declarava
29.006 relocations; o maior limite observado no conjunto era 175.680. O relatório
continha 291.749 entradas de símbolos declaradas e 4.592 program headers.
O arquivo original e seus dados detalhados permanecem fora do Git.

Havia dois problemas independentes: a rejeição acima de 4.096 entradas antes da
leitura e o orçamento único de 16.384 registros, que cortava símbolos, identidades,
segmentos e estatísticas juntos. Depois de consumido por símbolos de bibliotecas,
o orçamento apagava também os segmentos do ShellCore. O plano de memória mostrava
zeros mesmo quando seus headers já haviam sido validados.

## Implementação

- Symbols e RELA são lidos em blocos de **1.024 entradas / 24 KiB**, inclusive
  tabelas SCE e ELF padrão. Não são alocados arrays proporcionais à tabela inteira.
- Tamanho de entrada, divisibilidade, ranges completos e limites de cada leitura
  continuam verificados. Endereço e largura de cada relocation são verificados;
  offset de arquivo não vira endereço virtual. Nenhuma relocation é aplicada.
- São mantidos limites por arquivo e por scan, em ParserBudget. O teto de entradas
  passou a medir trabalho permitido, sem exigir que a tabela caiba numa leitura.
- A retenção de símbolos tem orçamento próprio. Quando acaba, continua-se a
  examinar as entradas sob os limites de trabalho e a validar relocations;
  registros não guardados são contados, não inventados. O resultado fica
  incompleto, com erro e exit 1, mesmo que os metadados examinados sejam válidos.
- Segmentos, dependências e identidades têm armazenamento contabilizado separado.
  Não há mais resize/clear dessas estruturas porque os símbolos consumiram o
  orçamento. Se o orçamento estrutural acabar, interrompe-se aquela análise com
  diagnostic_limit_exceeded, preservando o que já foi coletado.
- O inventário é ordenado por caminho antes de distribuir os orçamentos. A seleção
  não depende da ordem arbitrária de enumeração do filesystem na mesma plataforma.
- A associação dos IDs de bibliotecas/módulos usa índices por ID, evitando percorrer
  todas as dependências para cada símbolo de tabelas grandes.
- O plano de boot usa entry e segmentos com segments_validated=true, mesmo se
  uma etapa posterior falhar. Acrescenta o bloqueio do parser e não afirma boot
  possível. Headers parciais ou inválidos não recebem esse marcador.

## Limites atuais

| Recurso | Teto |
| --- | ---: |
| Entradas por bloco de símbolos/RELA | 1.024 |
| Entradas de símbolos por arquivo | 131.072 |
| Relocations examinadas por arquivo, somando tabelas | 1.048.576 |
| Entradas de símbolos por scan | 1.048.576 |
| Relocations por scan | 8.388.608 |
| Verificações de segmentos por scan | 67.108.864 |
| Orçamento contabilizado de retenção de símbolos | 128 MiB |
| Orçamento contabilizado de estruturas essenciais | 16 MiB |

Os bytes contabilizados incluem reservas conservadoras para structs, crescimento
vetorial e strings. Não são uma medição ou teto exato do RSS do processo: há
buffers temporários, inventário, logs, grafo e serialização adicionais.
Não são guardadas relocations individuais: apenas contagens por tipo.
Os orçamentos de trabalho são debitados antes de processar cada bloco; um bloco
que não cabe é recusado. Assim os contadores de trabalho reservado podem exceder
as entradas efetivamente examinadas quando há erro no meio do bloco.

Permanece o teto individual de leitura de 4 MiB para os demais metadados,
inclusive tabelas de strings; nomes continuam limitados a 1.024 bytes. Headers,
identidades/dependências por arquivo, total de dependências/candidatos, caminhos,
VFS e max_entries mantêm as proteções anteriores. Ainda podem ocorrer limites.
A API de biblioteca permite reduzir os orçamentos para testes/embeddings, mas
rejeita valores acima desses máximos. Não há opção CLI para remover os tetos.

## Relatórios

O JSON schema 3 recebe campos adicionais, sem remover os anteriores:

- parser_revision: **bounded-tables-v1**.
- parser_limits: bloco, tetos por arquivo, orçamentos iniciais e remanescentes.
- elf.symbol_entries_examined e elf.symbol_records_omitted.
- elf.relocation_table_entries e elf.relocation_entries_examined.
- elf.segments_validated.

"Examined" descreve entradas visitadas, inclusive a entrada na qual um erro foi
identificado; não substitui status/notes. Os campos de relocation somam passagens
pelas tabelas declaradas. Tipos desconhecidos continuam unsupported_metadata.
Com retenção parcial, symbol_status é incomplete_metadata_limit, scan_complete
é false e o analisador mantém analysis.complete=false. Nenhum sucesso funcional
é inferido de símbolos armazenados ou de metadados preservados.
O texto inclui revisão, orçamento remanescente e contadores por arquivo.

## Validação local

Build Debug e Release com Clang, e GCC Debug com ASan/UBSan: **seis entradas
CTest passaram nas três configurações**. Scanner: 103 casos, três exclusivos
Windows ignorados; gerador: cinco casos; quatro verificadores C++.
LeakSanitizer permanece desativado por restrição do sandbox (detect_leaks=0).
clang-format 23 e git diff --check passaram, sem warnings nos builds locais.

Regressões novas cobrem:

- 29.006 e 175.680 relocations, incluindo tabela maior que 4 MiB, com último
  registro não vazio e validação de seu destino.
- Destino inválido no último bloco e string inválida depois do primeiro bloco.
- 28.269 símbolos e tabelas de 4.097 entradas antes rejeitadas.
- Tetos por arquivo excedidos e interrupção por trabalho agregado de símbolos,
  relocations e validação de segmentos.
- Retenção esgotada em uma biblioteca anterior, sem apagar segmentos,
  dependências/identidade do Core posterior ou impedir validação de relocations.
- Mais de 16.384 símbolos no corpus, preservando os dados de arquivos posteriores.
- Orçamento estrutural zero, recusa de tetos ampliados e exportação de relatórios
  incompletos sem tratar o resultado como boot permitido.

Os fixtures são sintéticos: os números reproduzem as escalas observadas, não
conteúdo Sony. Logs locais ignorados: .cloud-env/logs/bounded-*.log.
Não houve execução de firmware, mudança no núcleo do emulador ou nos backends
Windows/Linux. Windows/MSVC permanece pendente de CI/validação local; não foi
executado neste host. O teste novo também entra no workflow Windows existente.

## Repetir o teste no Windows

```powershell
git fetch origin
git switch fix/dashboard-900-bounded-tables
cmake -S .\tools\firmware900 -B .\build-firmware900 -DPISTATION_ENABLE_TESTS=ON
cmake --build .\build-firmware900 --config Debug --parallel 4
ctest --test-dir .\build-firmware900 -C Debug --output-on-failure

$outDir = Join-Path $env:TEMP ("pistation-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $outDir | Out-Null
$report = Join-Path $outDir "scan.json"
& ".\build-firmware900\Debug\pistation-firmware900.exe" `
    --firmware-dir $firmware --report $report --boot-diagnostic
```

Confirmar parser_revision antes de comparar os resultados. O JSON pode crescer
porque agora guarda mais símbolos. Para compartilhar somente o diagnóstico
principal, sem incluir o inventário completo de símbolos:

```powershell
$r = Get-Content -Raw -Encoding UTF8 $report | ConvertFrom-Json
[ordered]@{
    parser_revision = $r.parser_revision
    parser_limits = $r.parser_limits
    scan_complete = $r.scan_complete
    error_count = $r.error_count
    shellcore = $r.files | Where-Object { $_.path -eq 'system/vsh/SceShellCore.elf' } |
        Select-Object path,status,symbol_status,notes,dependencies,elf
} | ConvertTo-Json -Depth 30 |
    Set-Content -Encoding UTF8 (Join-Path $outDir 'shellcore-summary.json')
```

Só esse novo scan poderá confirmar se os 488 diagnósticos anteriores desapareceram
e quais bloqueios reais restam. Não se presume que as escalas sejam a única
limitação dos executáveis reais. --attempt-boot continua bloqueado por falta de
backend nativo; nada nesta alteração implementa execução do dashboard.

## Arquivos

firmware900_parser_budget.h, elf_metadata_parser.cpp/.h,
firmware900_manager.cpp/.h, firmware900_boot_diagnostic.cpp, CMakeLists.txt,
tests/test_scanner.py, tests/test_services.cpp e tests/test_parser_budgets.cpp,
todos em tools/firmware900; este documento em docs/pistation.
