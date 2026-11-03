# PiStation PS4 — Fase 4

A Fase 4 entrega um diagnóstico reproduzível do impedimento de execução,
sem afirmar que houve uma tentativa de executar código convidado.

A ferramenta das Fases 1–3 continua independente. Firmware900Manager,
ServiceManager, analisador, VFS e plano de memória são reutilizados.
O linker nativo não está vinculado à CLI offline. A compilação integral permanece
bloqueada pelo fdk-aac original ausente (FDK_delay.cpp); os logs anteriores e a
nova configuração CMake confirmam esse impedimento.

## Mudança mínima

`--attempt-boot` solicita explicitamente o modo
`PS4 Firmware 9.00 Experimental Boot`. Desativado por padrão, ele executa a
inspeção existente e encerra com código 1, registrando `[PS4_BOOT]` e o primeiro
impedimento da preparação nativa: `boot_backend_unavailable`.
A última operação registrada é `prepare_native_execution_backend`.
JSON e texto são exportados pelos mecanismos existentes. `boot_attempted=false`
continua verdadeiro quanto à ausência de execução: solicitar não significa executar.

Esse modo é uma barreira diagnóstica, não um backend de boot implementado.
Não prepara memória convidada ou chama Linker sem o runtime necessário.
Não há inicialização gráfica, processos ou serviços convidados novos.
Não foram acrescentados retornos fictícios de sucesso ou descriptografia.

## Verificação

O teste de regressão usa ELF sintético e exige saída de erro, evento impeditivo
e ausência de execução. A suíte relacionada e a compilação Debug da ferramenta
foram executadas; evidências em `.cloud-env/logs/phase04-build.log` e
`phase04-tests.log`. CMake do núcleo: `phase04-core.log`.
Não há testes reais de dashboard ou jogos. Nenhum arquivo Sony foi usado.

Reprodução após compilar a ferramenta:

```bash
pistation-firmware900 --firmware-dir /pasta/local --attempt-boot \
  --report /fora/da/pasta/novo.json --text-report /fora/da/pasta/novo.txt
```

Arquivos alterados: tools/firmware900/main.cpp,
tools/firmware900/tests/test_scanner.py e este documento.
Nenhum código do caminho normal de jogos foi modificado; regressão funcional
com jogos não foi validada. Nenhum firmware foi versionado.

## Próximo passo

Aplicar a liberação de android.googlesource.com e completar o submódulo original;
compilar/testar o núcleo. Em seguida vincular uma entrada experimental ao runtime
existente e testar um ELF sintético controlado antes de qualquer componente real.
Ainda faltam imports reais, contratos IPC/multiprocessos, parâmetros de processo,
seleção HLE/LLE e validação gráfica. SceShellUI/Core reais não foram analisados.
Dashboard 9.00 não iniciado; Fase 4 limitada ao diagnóstico do bloqueio.
