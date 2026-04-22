import typer
from typer.testing import CliRunner

app = typer.Typer()

@app.command()
def main(
    name: str @ typer.Argument(..., help="The person to greet"),
    count: int @ typer.Option(help="Number of greetings") = 1,
    formal: bool @ typer.Option("--formal/--informal") = False
):
    for _ in range(count):
        if formal:
            print(f"Greetings, Mx. {name}.")
        else:
            print(f"Hello {name}!")

res = CliRunner().invoke(app, ["Alice", "--count", "5"])
print(res.stdout)
